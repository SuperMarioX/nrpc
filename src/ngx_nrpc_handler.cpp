#include <io/iobuf_zero_copy_stream.h>
#include "ngxplus_iobuf.h"
#include "ngxplus_timer.h"
#include "ngx_nrpc_handler.h"
#include "controller.h"
#include "protocol.h"

namespace nrpc {

void ngx_nrpc_finalize_session(Controller* cntl)
{
	cntl->finalize();
	delete cntl;
}

void ngx_nrpc_close_connection(ngx_connection_t* c)
{
    c->destroyed = 1;

    // this function will delete the timer and event,
    // recycle the connection to free_queue
    ngx_close_connection(c);
    return;
}

void ngx_nrpc_init_connection(ngx_connection_t *c)
{
    auto cntl = new Controller();
    if (!cntl->server_side_init(c)) {
        return ngx_nrpc_finalize_session(cntl);
    }
    c->data = (void*)cntl;

    c->read->handler = ngx_nrpc_determine_policy;
    return ngx_nrpc_determine_policy(c->read);
}

void ngx_nrpc_reuse_connection(ngx_event_t *rev)
{
    ngx_connection_t *c = (ngx_connection_t*)rev->data;

    if (rev->timedout) {
        // no data from client reuse the connection
        return ngx_nrpc_close_connection(c);
    }
    // re-init the connection, new Controller
    return ngx_nrpc_init_connection(c);
}

// read one byte to determin policy
// just finalize when (timeout | eof | error)
void ngx_nrpc_determine_policy(ngx_event_t *rev)
{
    ngx_connection_t *c = (ngx_connection_t*)rev->data;
    Controller* cntl = (Controller*)c->data;

    if (rev->timedout) {
        c->timedout = 1;
        cntl->set_result(RPC_READ_TIMEOUT);
        return ngx_nrpc_finalize_session(cntl);
    }

    char first_character;
    int n = c->recv(c, (u_char*)&first_character, 1);
    if(n == NGX_AGAIN) {
        if (!rev->timer_set) {
            ngx_add_timer(rev, cntl->server_read_timeout() * 1000);
        }
        if (ngx_handle_read_event(rev, 0) != NGX_OK) {
            cntl->set_result(RPC_INNER_ERROR);
            cntl->set_result_text("hanlde read event error");
            return ngx_nrpc_finalize_session(cntl);
        }
        // for next event
        return;
    }

    // read eof means the session don't start(the client channel destructs early)
    // OR the connection isn't reused more
	// SO don't need finalize_session, just close_connection, delete cntl, then return
    if (n == 0) {
        ngx_nrpc_close_connection(c);
        delete cntl/*this is a empty cntl now*/;
		LOG(NOTICE, "connection close before send anything");
		return/*don't continue*/;
    }
    if (n == NGX_ERROR) {
        cntl->set_result(RPC_READ_ERROR);
        return ngx_nrpc_finalize_session(cntl);
    }

    // n == 1, determin the protocol
    c->received += 1;
    int protocol_num;
    switch(first_character) {
    case 'N':
        protocol_num = NRPC_PROTOCOL_DEFAULT_NUM/*default*/;
        break;
    case 'G':
    case 'P':
    case 'H':
        protocol_num = NRPC_PROTOCOL_HTTP_NUM/*http*/;
        break;
    default:
        protocol_num = 100/*invaild*/;
        break;
    }
    if (!cntl->set_protocol(protocol_num/*default_protocol*/)) {
        return ngx_nrpc_finalize_session(cntl);
    }

    // get iobuf, and read the following data
    ngxplus::NgxplusIOBuf* iobuf = new ngxplus::NgxplusIOBuf(); // default blocksize 1024
    char* buf;
    int len = iobuf->alloc(&buf, 1, common::IOBUF_ALLOC_EXACT);
    if (len != 1) {
        cntl->set_result(RPC_INNER_ERROR);
        cntl->set_result_text("determin alloc error");
        return ngx_nrpc_finalize_session(cntl);
    }
    buf[0] = first_character;
    cntl->set_iobuf(iobuf);

    rev->handler = ngx_nrpc_read_request;
    return ngx_nrpc_read_request(rev);
}

void ngx_nrpc_read_request(ngx_event_t *rev)
{
    ngx_connection_t *c = (ngx_connection_t*)rev->data;
    Controller *cntl = (Controller*)c->data;

    if (rev->timedout) {
        c->timedout = 1;
        cntl->set_result(RPC_READ_TIMEOUT);
        return ngx_nrpc_finalize_session(cntl);
    }

    RPC_SESSION_STATE cntl_state = cntl->state();
    if (cntl_state != RPC_SESSION_READING) {
        cntl->set_result(RPC_INNER_ERROR);
        cntl->set_result_text("should not come into read request");
        return ngx_nrpc_finalize_session(cntl);
    }

    ngxplus::NgxplusIOBuf *iobuf = cntl->iobuf();
    common::IOBufAsZeroCopyOutputStream zero_out_stream(iobuf);

    char* buf;
    int size;
    for(;;) {
        if (!zero_out_stream.Next((void**)&buf, &size)) {
            cntl->set_result(RPC_INNER_ERROR);
            cntl->set_result_text("read request alloc error");
            return ngx_nrpc_finalize_session(cntl);
        }
        if (size == 0) {
            continue;
        }
        int len = c->recv(c, (u_char*)buf, size);
        if (len == size) {
            continue;
        }
        if (len == NGX_ERROR) {
            cntl->set_result(RPC_READ_ERROR);
            return ngx_nrpc_finalize_session(cntl);
        }

        // read eof or again
        // if again, also parse it, and ignore the follow stream if PARSE_DONE
        bool read_eof = false;
        if (len == 0) {
            read_eof = true;
        }
        Protocol* protocol = cntl->protocol();
        ParseResult presult = protocol->parse(cntl, read_eof);
        if (presult == PARSE_DONE) {
            rev->handler = ngx_nrpc_dummy_read;
            if (rev->timer_set) {
                ngx_del_timer(rev);
            }
            cntl->set_state(RPC_SESSION_PROCESSING);
            cntl->set_start_process_time_us(ngxplus::rawtime_us());
            return protocol->process_request(cntl);
        }
        else if (presult == PARSE_INCOMPLETE) {
            ngx_add_timer(rev, cntl->server_read_timeout() * 1000);
            if (ngx_handle_read_event(rev, 0) != NGX_OK) {
                cntl->set_result(RPC_INNER_ERROR);
                cntl->set_result_text("hanlde read event error");
                return ngx_nrpc_finalize_session(cntl);
            }
            return;
        }
        else { // bad format OR broken OR bad meta
            cntl->set_result(RPC_READ_ERROR);
            if (presult == PARSE_BROKEN) {
                cntl->set_result_text("read broken");
            }
            if (presult == PARSE_BAD_FORMAT) {
                cntl->set_result_text("request bad format");
            }
            return ngx_nrpc_finalize_session(cntl);
        }
    }
}

void ngx_nrpc_send_response(ngx_event_t* wev)
{
    ngx_connection_t* c = (ngx_connection_t*)wev->data;
    Controller* cntl = (Controller*)c->data;
    ngxplus::NgxplusIOBuf* iobuf = cntl->iobuf();

    common::IOBufAsZeroCopyInputStream zero_in_stream(iobuf);
    char* buf;
    int size;
    while (zero_in_stream.Next((const void**)&buf, &size)) {
        if (size == 0) {
            continue;
        }
        int n = c->send(c, (u_char*)buf, size);
        if (n < size) {
            if (!wev->timer_set) {
                ngx_add_timer(wev, cntl->server_write_timeout() * 1000);
            }
            if (ngx_handle_write_event(wev, 0) != NGX_OK) {
                cntl->set_result(RPC_INNER_ERROR);
                cntl->set_result_text("hanlde write event error");
                return ngx_nrpc_finalize_session(cntl);
            }
            // for the next event_loop
            return;
        }
        // n == size, read next
    }
    // send all to tcp buf
	// Don't set RPC_OK, _result inited by RPC_OK, but in the processing procedure
	// it may be seted to be RPC_PROCESS_ERROR, we also need to send the resp, and
	// log this error
    //cntl->set_result(RPC_OK);
    cntl->set_state(RPC_SESSION_LOGING);
    cntl->set_end_time_us(ngxplus::rawtime_us());
    return ngx_nrpc_finalize_session(cntl);
}

void ngx_nrpc_dummy_read(ngx_event_t* rev)
{
    (void) rev;
    return;
}

void ngx_nrpc_dummy_write(ngx_event_t* wev)
{
    (void) wev;
    return;
}

}
