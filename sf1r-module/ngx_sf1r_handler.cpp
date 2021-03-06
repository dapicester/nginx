/* 
 * File:   ngx_sf1r_handler.cpp
 * Author: Paolo D'Apice
 * 
 * Created on January 17, 2012, 11:23 AM
 */

#define BOOST_THREAD_DONT_USE_CHRONO

extern "C" {
#include "ngx_sf1r_handler.h"
#include "ngx_sf1r_module.h"
#include "ngx_sf1r_utils.h"
}
#include "ngx_sf1r_ddebug.h"
#include <3rdparty/zookeeper/ZooKeeper.hpp>
#include <net/sf1r/Sf1DriverBase.hpp>
#include <string>

using NS_IZENELIB_SF1R::ClientError;
using NS_IZENELIB_SF1R::NetworkError;
using NS_IZENELIB_SF1R::RoutingError;
using NS_IZENELIB_SF1R::ServerError;
using NS_IZENELIB_SF1R::Sf1DriverBase;
using std::string;


static ngx_str_t TOKENS_HEADER = ngx_string(SF1_TOKENS_HEADER);


/// Callback called after getting the request body.
static void ngx_sf1r_request_body_handler(ngx_http_request_t*);

/// Checks if the request must be processed.
static ngx_flag_t ngx_sf1r_check_request_body(ngx_http_request_t*);

/// Sends the response.
static ngx_int_t ngx_sf1r_send_response(ngx_http_request_t*, ngx_uint_t, ngx_sf1r_ctx_t*);


ngx_int_t
ngx_sf1r_handler(ngx_http_request_t* r) {
    // response to 'GET' and 'POST' requests only
    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_POST))) {
        ddebug("HTTP method not allowed, discarding");
        return NGX_HTTP_NOT_ALLOWED;
    }
    
    // discard header only requests
    if (r->header_only) {
        ddebug("header only request, discarding");
        return NGX_HTTP_BAD_REQUEST;
    }

    // set context
    ngx_sf1r_ctx_t* ctx = scast(ngx_sf1r_ctx_t*, ngx_pcalloc(r->pool, sizeof(ngx_sf1r_ctx_t)));
    if (ctx == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->pool->log, 0, "failed to allocate memory");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_sf1r_module);
    ddebug("request contex set");
    
    /* process header */
    
    ctx->body_len = r->headers_in.content_length_n;
    ddebug("   len: [%zu]", ctx->body_len);
    
    // get URI
    
    ctx->uri.data = r->uri.data;
    ctx->uri.len = r->uri.len;
    ddebug("   uri: [%s]", dsubstr(ctx->uri));
    
    // get tokens
    
    ngx_list_part_t* part = &r->headers_in.headers.part;
    ngx_table_elt_t* h = scast(ngx_table_elt_t*, part->elts);
    
    for (ngx_uint_t i = 0; ; ++i) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            h = scast(ngx_table_elt_t*, part->elts);
            i = 0;
        }
        
        if (h[i].key.len == TOKENS_HEADER.len
                and ngx_strncasecmp(h[i].key.data,
                                    TOKENS_HEADER.data,
                                    TOKENS_HEADER.len) == 0) {
            ctx->tokens.data = h[i].value.data;
            ctx->tokens.len = h[i].value.len;
            
            break;
        }
    }
    if (ctx->tokens.len == 0) ctx->tokens.data = (u_char*) "";
    ddebug("tokens: [%s]", ctx->tokens.data);
    
    /* process body */
    
    ngx_int_t rc = ngx_http_read_client_request_body(r, ngx_sf1r_request_body_handler);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ddebug("got special response: %d", (int) rc);
        return rc;
    }
    
    return NGX_DONE;
}


static inline ngx_flag_t
ngx_sf1r_check_request_body(ngx_http_request_t* r) {
    if (r->request_body == NULL) {
        ddebug("request body is NULL");
        return NGX_ERROR;
    }
    if (r->request_body->bufs == NULL) {
        ddebug("request body buffer is NULL");
        return NGX_ERROR;
    }
    return NGX_OK;
}


static void 
ngx_sf1r_request_body_handler(ngx_http_request_t* r) {
    if (ngx_sf1r_check_request_body(r) != NGX_OK) {
        ddebug("no body in request, discarding");
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }
    
    ngx_sf1r_ctx_t* ctx = scast(ngx_sf1r_ctx_t*, ngx_http_get_module_ctx(r, ngx_sf1r_module));
    
    /* do actual processing */
    
    ddebug("reading request body ...");
    
    string body;
    ngx_chain_t* cl = r->request_body->bufs;
    
    ngx_buf_t* buf = cl->buf;
    size_t len = buf->last - buf->pos;
    ddebug("first buffer: %zu/%zu", len, ctx->body_len);
    body.assign(rcast(char*, buf->pos), len);
    ddebug("first buffer content:\n%s\n", body.c_str());
    
    if (cl->next != NULL) {
        ddebug("reading from the second buffer ...");
        
        if (r->request_body->temp_file) {
            len = ctx->body_len - len;
            u_char buffer[len];
            ngx_read_file(&r->request_body->temp_file->file, buffer, len, 0);
            ddebug("file buffer: %zu/%zu", len, ctx->body_len);
            body.append(rcast(char*, buffer), len);
            ddebug("file buffer content:\n%s\n", buffer);
        } else {
            ngx_buf_t* next = cl->next->buf;
            len = next->last - next->pos;
            ddebug("memory buffer: %zu/%zu", len, ctx->body_len);
            
            body.append(rcast(char*, next->pos), len);
            ddebug("memory buffer contet:\n%s\n", string(rcast(char*, next->pos), len).c_str());
        }
    }

    ddebug("full request body:\n%s\n", body.c_str());
    
    ngx_sf1r_loc_conf_t* conf = scast(ngx_sf1r_loc_conf_t*, ngx_http_get_module_loc_conf(r, ngx_sf1r_module));
    
    ngx_int_t rc;
    try {
        ddebug("sending request and getting response to SF1 ...");
        Sf1DriverBase* driver = scast(Sf1DriverBase*, conf->driver);
        
        string uri(rcast(char*, ctx->uri.data), ctx->uri.len);
        string tokens(rcast(char*, ctx->tokens.data), ctx->tokens.len);
        
        string response = driver->call(uri, tokens, body);
        ddebug("response body:\n%s\n", response.c_str());
        
        // cannot use use the char* inside string, because it will raise a Bad Address (14) error.
        ctx->response_len = response.length();
        ctx->response_body = scast(char*, ngx_pcalloc(r->pool, response.length()));
        response.copy(ctx->response_body, response.length());
        
        /* send response */
        rc = ngx_sf1r_send_response(r, NGX_HTTP_OK, ctx);
        
    } catch (ClientError& e) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ClientError: %s", e.what());
        rc = NGX_HTTP_BAD_REQUEST;
    } catch (ServerError& e) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ServerError: %s", e.what());
        rc = NGX_HTTP_BAD_GATEWAY;
    } catch (RoutingError& e) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "RoutingError: %s", e.what());
        rc = NGX_HTTP_SERVICE_UNAVAILABLE;
    } catch (NetworkError& e) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "NetworkError: %s", e.what());
        rc = NGX_HTTP_GATEWAY_TIME_OUT;
    } catch (std::exception& e) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Exception: %s", e.what());
        rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    
    ngx_http_finalize_request(r, rc);
}
    

static ngx_int_t 
ngx_sf1r_send_response(ngx_http_request_t* r, ngx_uint_t status, ngx_sf1r_ctx_t* ctx) {
    ddebug("sending response ...");
    
    /* response header */
    
    // set the status line
    r->headers_out.status = status;
    r->headers_out.content_length_n = ctx->response_len;
    // TODO: set content type according to Sf1Config (parameter: JSON/XML)
    r->headers_out.content_type_len = sizeof(APPLICATION_JSON) - 1;
    r->headers_out.content_type.len = sizeof(APPLICATION_JSON) - 1;
    r->headers_out.content_type.data = (u_char*) APPLICATION_JSON;
    
    // send the header
    ngx_int_t rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }
    ddebug("sent response header: [%zu - %s]", r->headers_out.status = status, r->headers_out.content_type.data);
    
    /* response body */
    
    // allocate a buffer
    ngx_buf_t* buffer = scast(ngx_buf_t*, ngx_calloc_buf(r->pool));
    if (buffer == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Failed to allocate response body buffer.");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    
    // adjust the pointers of the buffer
    buffer->start = buffer->pos = (u_char*) ctx->response_body;
    buffer->end = buffer->last = (u_char*) (ctx->response_body + ctx->response_len);
    
    // buffer flags
    buffer->memory = 1;
    buffer->last_buf = 1;
    buffer->last_in_chain = 1;

    // attach this buffer to the buffer chain
    ngx_chain_t out;
    out.buf = buffer;
    out.next = NULL;
    ddebug("response buffer set");
    
    return ngx_http_output_filter(r, &out);
}
