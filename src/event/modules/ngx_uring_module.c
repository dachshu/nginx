/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include "liburing.h"

ngx_uint_t                  ngx_use_uring_rdhup = 1;

typedef struct uring_splice_pipe {
    int                 pipe_fds[2];
    struct uring_splice_pipe  *next;
} uring_splice_pipe;

typedef struct uring_conn_info {
    ngx_connection_t   *conn;
    uring_splice_pipe  *splice_pipe;  
    struct uring_conn_info    *next;            
    uint16_t            rq_type;
} uring_conn_info;

enum {
    NGX_URING_ACCEPT = 0,
    NGX_URING_READ,
    NGX_URING_WRITE,
    NGX_URING_WRITEV,
    NGX_URING_SPLICE_IN,
    NGX_URING_SPLICE_OUT,
    NGX_URING_NOTIFY_READ
};


typedef struct ngx_uring_conf_t {
    ngx_uint_t entries;
} ngx_uring_conf_t;


static uring_conn_info *uring_conn_infos;
static uring_conn_info *get_uring_conn_info();
static void return_uring_conn_info(uring_conn_info* conn_info);

#if (0)
static uring_splice_pipe *uring_splice_pipes;
static uring_splice_pipe *get_uring_splice_pipe();
static void return_uring_splice_pipe(uring_splice_pipe* p);
#endif


#define NGX_SENDFILE_MAXSIZE  2147483647L

static ngx_int_t ngx_uring_init(ngx_cycle_t *cycle, ngx_msec_t timer);
// #if (NGX_HAVE_EVENTFD)
// static ngx_int_t ngx_uring_notify_init(ngx_log_t *log);
// static void ngx_uring_notify_handler(ngx_event_t *ev);
// #endif
static void ngx_uring_done(ngx_cycle_t *cycle);
static ngx_int_t ngx_uring_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_uring_add_connection(ngx_connection_t *c);
static ngx_int_t ngx_uring_del_connection(ngx_connection_t *c,
    ngx_uint_t flags);
// #if (NGX_HAVE_EVENTFD)
// static ngx_int_t ngx_uring_notify(ngx_event_handler_pt handler);
// #endif
static ngx_int_t ngx_uring_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
    ngx_uint_t flags);
static void *ngx_uring_create_conf(ngx_cycle_t *cycle);
static char *ngx_uring_init_conf(ngx_cycle_t *cycle, void *conf);


static ssize_t ngx_uring_recv(ngx_connection_t *c, u_char *buf, size_t size);
#if (0)
static ngx_chain_t *ngx_uring_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit);
static ssize_t ngx_uring_splice_sendfile(ngx_connection_t *c, ngx_buf_t *file, size_t size, void *pool_data);
#endif
static ssize_t ngx_uring_writev(ngx_connection_t *c, int nelts, int start_el, void *pool_data);
ngx_chain_t * ngx_uring_writev_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit);

static struct io_uring ring;


// #if (NGX_HAVE_EVENTFD)
// static int                  notify_fd = -1;
// static ngx_event_t          notify_event;
// static ngx_connection_t     notify_conn;
// static uint64_t             notify_count_buf;
// static uring_conn_info      notify_conn_info;
// #endif

static ngx_str_t      uring_name = ngx_string("io_uring");

static ngx_command_t  ngx_uring_commands[] = {

    { ngx_string("uring_entries"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_uring_conf_t, entries),
      NULL },

      ngx_null_command
};


static ngx_event_module_t  ngx_uring_module_ctx = {
    &uring_name,
    ngx_uring_create_conf,               /* create configuration */
    ngx_uring_init_conf,                 /* init configuration */

    {
        ngx_uring_add_event,             /* add an event */
        NULL,                            /* delete an event */
        ngx_uring_add_event,             /* enable an event */
        NULL,                            /* disable an event */
        ngx_uring_add_connection,        /* add an connection */
        ngx_uring_del_connection,        /* delete an connection */
// #if (NGX_HAVE_EVENTFD)
//         ngx_uring_notify,                /* trigger a notify */
// #else
        NULL,                            /* trigger a notify */
//#endif
        ngx_uring_process_events,        /* process the events */
        ngx_uring_init,                  /* init the events */
        ngx_uring_done,                  /* done the events */
    }
};

//ngx_module_t  ngx_uring_module = {
ngx_module_t  ngx_uring_module = {
    NGX_MODULE_V1,
    &ngx_uring_module_ctx,               /* module context */
    ngx_uring_commands,                  /* module directives */
    NGX_EVENT_MODULE,                    /* module type */
    NULL,                                /* init master */
    NULL,                                /* init module */
    NULL,                                /* init process */
    NULL,                                /* init thread */
    NULL,                                /* exit thread */
    NULL,                                /* exit process */
    NULL,                                /* exit master */
    NGX_MODULE_V1_PADDING
};

// udp is not implemented yet
ngx_os_io_t ngx_uring_io = {
    ngx_uring_recv,
    NULL, //ngx_uring_readv_chain,////////////
    NULL,
    NULL, //ngx_uring_send,/////////////////
    NULL,
    NULL,
#if (0)
    ngx_uring_sendfile_chain,
    NGX_IO_SENDFILE
#else
    ngx_uring_writev_chain,
    0
#endif
};




static ngx_int_t
ngx_uring_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ngx_write_console(ngx_stderr, "ngx_uring_init()\n", strlen("ngx_uring_init()\n"));
    ngx_uring_conf_t  *epcf;
    long unsigned int i;

    epcf = ngx_event_get_conf(cycle->conf_ctx, ngx_uring_module);

    
    //if (ring.ring_fd == -1 || ring_params.cq_entries == 0 || ring_params.sq_entries == 0) {
    if (ring.sq.ring_sz == 0){
        if (io_uring_queue_init(epcf->entries, &ring, 0) < 0) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "io_uring_queue_init() failed");
            return NGX_ERROR;
        }


// #if (NGX_HAVE_EVENTFD)
//         if (ngx_uring_notify_init(cycle->log) != NGX_OK) {
//             ngx_uring_module_ctx.actions.notify = NULL;
//         }
// #endif
    }

    uring_conn_infos = NULL;
    for(i = 0; i < epcf->entries * 2; ++i){
        uring_conn_info *nc = (uring_conn_info*)malloc(sizeof(uring_conn_info));
        if(nc == NULL) return NGX_ERROR;
        nc->conn = NULL;
        nc->splice_pipe = NULL;
        nc->next = uring_conn_infos;
        uring_conn_infos = nc;
    }
    #if (0)
    uring_splice_pipes = NULL;
    for(i = 0; i < epcf->entries / 2; ++i){
        uring_splice_pipe *np = (uring_splice_pipe*)malloc(sizeof(uring_splice_pipe));
        if(np == NULL) return NGX_ERROR;
        if(pipe(np->pipe_fds) < 0){
            return NGX_ERROR;
        }
        np->next = uring_splice_pipes;
        uring_splice_pipes = np;
    }
    #endif

    ngx_io = ngx_uring_io;

    ngx_event_actions = ngx_uring_module_ctx.actions;

#if (NGX_HAVE_CLEAR_EVENT)
    ngx_event_flags = NGX_USE_CLEAR_EVENT
#else
    ngx_event_flags = NGX_USE_LEVEL_EVENT
#endif
                      |NGX_USE_GREEDY_EVENT
                      |NGX_USE_EPOLL_EVENT
                      |NGX_USE_URING_EVENT;
    //ngx_event_flags = NGX_USE_URING_EVENT;


    return NGX_OK;
}


// #if (NGX_HAVE_EVENTFD)

// static ngx_int_t
// ngx_uring_notify_init(ngx_log_t *log)
// {

// #if (NGX_HAVE_SYS_EVENTFD_H)
//     notify_fd = eventfd(0, 0);
// #else
//     notify_fd = syscall(SYS_eventfd, 0);
// #endif

//     if (notify_fd == -1) {
//         ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "eventfd() failed");
//         return NGX_ERROR;
//     }

//     ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
//                    "notify eventfd: %d", notify_fd);

//     notify_event.handler = ngx_uring_notify_handler;
//     notify_event.log = log;
//     notify_event.active = 1;

//     notify_conn.fd = notify_fd;
//     notify_conn.read = &notify_event;
//     notify_conn.log = log;

//     struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);

//     io_uring_prep_recv(sqe, notify_fd, &notify_count_buf, sizeof(uint64_t), 0);

//     notify_conn_info.conn = &notify_conn;
//     notify_conn_info.rq_type = NGX_URING_NOTIFY_READ;
    
// 	io_uring_sqe_set_data(sqe, &(notify_conn_info));

//     return NGX_OK;
// }


// static void
// ngx_uring_notify_handler(ngx_event_t *ev)
// {  
//     ssize_t               n;
//     ngx_err_t             err;
//     ngx_event_handler_pt  handler;
//     if (++ev->index == NGX_MAX_UINT32_VALUE) {
//         ev->index = 0;

//         n = ev->uring_res;

//         err = ngx_errno;

//         ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
//                        "read() eventfd %d: %z count:%uL", notify_fd, n, notify_count_buf);

//         if ((size_t) n != sizeof(uint64_t)) {
//             ngx_log_error(NGX_LOG_ALERT, ev->log, err,
//                           "read() eventfd %d failed", notify_fd);
//         }
//     }

//     handler = ev->data;
//     handler(ev);
// }

// #endif



static void
ngx_uring_done(ngx_cycle_t *cycle)
{
    io_uring_queue_exit(&ring);
    ring.ring_fd = -1;

// #if (NGX_HAVE_EVENTFD)

//     if (close(notify_fd) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "eventfd close() failed");
//     }

//     notify_fd = -1;

// #endif
    uring_conn_info *c;
    while(uring_conn_infos)
    {
        c = uring_conn_infos;
        uring_conn_infos = uring_conn_infos->next;
        free(c);
    }
    #if (0)
    uring_splice_pipe *p;
    while(uring_splice_pipes)
    {
        p = uring_splice_pipes;
        uring_splice_pipes = uring_splice_pipes->next;
        if(p->pipe_fds[0] >= 0){
            close(p->pipe_fds[0]);
            close(p->pipe_fds[1]);
        }
        free(p);
    }
    #endif
}


static ngx_int_t
ngx_uring_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    //ngx_write_console(ngx_stderr, "ngx_uring_add_event()\n", strlen("ngx_uring_add_event()\n"));
    ngx_connection_t    *c;

    c = ev->data;

    c->read->active = 1;
    c->write->active = 1;

    if(c->read->accept){
        if(c->sockaddr == NULL){
            c->sockaddr = malloc(sizeof(ngx_sockaddr_t));
            c->socklen = sizeof(ngx_sockaddr_t);
        }
         
        //printf("io_uring prep accept %d\n", c->fd);
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if(!sqe){
            ngx_connection_error(c, 0, "uring_writev() failed");
            return NGX_ERROR;
        }
        io_uring_prep_accept(sqe, c->fd, c->sockaddr, &c->socklen, 0);

        uring_conn_info *conn_info = get_uring_conn_info();
        conn_info->conn = c;
        conn_info->rq_type = NGX_URING_ACCEPT;
        io_uring_sqe_set_data(sqe, conn_info);
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "io_uring add event: fd:%d",
                   c->fd);

    return NGX_OK;
}


static ngx_int_t
ngx_uring_add_connection(ngx_connection_t *c)
{
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "io_uring add connection: fd:%d", c->fd);

    c->read->active = 1;
    c->write->active = 1;

    if(c->read->accept){
        if(c->sockaddr == NULL){
            c->sockaddr = ngx_palloc(c->pool, sizeof(ngx_sockaddr_t));
            c->socklen = sizeof(ngx_sockaddr_t);
        }

        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_accept(sqe, c->fd, c->sockaddr, &c->socklen, 0);

        uring_conn_info *conn_info = get_uring_conn_info();
        conn_info->conn = c;
        conn_info->rq_type = NGX_URING_ACCEPT;
        io_uring_sqe_set_data(sqe, conn_info);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_uring_del_connection(ngx_connection_t *c, ngx_uint_t flags)
{
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "io_uring del connection: fd:%d", c->fd);

    c->read->active = 0;
    c->write->active = 0;

    return NGX_OK;
}


// #if (NGX_HAVE_EVENTFD)

// static ngx_int_t
// ngx_uring_notify(ngx_event_handler_pt handler)
// {
//     static uint64_t inc = 1;

//     notify_event.data = handler;

//     if ((size_t) write(notify_fd, &inc, sizeof(uint64_t)) != sizeof(uint64_t)) {
//         ngx_log_error(NGX_LOG_ALERT, notify_event.log, ngx_errno,
//                       "write() to eventfd %d failed", notify_fd);
//         return NGX_ERROR;
//     }

//     return NGX_OK;
// }

// #endif


static ngx_int_t
ngx_uring_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    //ngx_write_console(ngx_stderr, "ngx_uring_process_events\n", strlen("ngx_uring_process_events\n"));
    ngx_event_t       *rev, *wev;
    ngx_queue_t       *queue;
    ngx_connection_t  *c;

    /* NGX_TIMER_INFINITE == INFTIM */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "io_uring timer: %M", timer);
    
    if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm) {
        ngx_time_update();
    }

    // setsockopt(s, IPPROTO_TCP, TCP_CORK,
    //                   (const void *) &cork, sizeof(int));

    io_uring_submit_and_wait(&ring, 1);
    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned count = 0;
    
    // go through all CQEs
    io_uring_for_each_cqe(&ring, head, cqe) {
        ++count;
        uring_conn_info* conn_info;
        conn_info = (uring_conn_info*)cqe->user_data;
        uint16_t rq_type = conn_info->rq_type;
        c = conn_info->conn;
        rev = c->read;
        wev = c->write;

        if (c->fd == -1) {
            /*
             * the stale event from a file descriptor
             * that was just closed in this iteration
             */
            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "io_uring: stale event %p", c);
            continue;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "io_uring: fd:%d rq:%d d:%p",
                       c->fd, rq_type, event_list[i].data.ptr);

        switch (rq_type)
        {
        case NGX_URING_ACCEPT:{
            int sock_conn_fd = cqe->res;
            rev->uring_res = sock_conn_fd;

            rev->ready = 1;
            rev->complete = 1;
            // if (flags & NGX_POST_EVENTS) {
            //     queue = rev->accept ? &ngx_posted_accept_events
            //                         : &ngx_posted_events;

            //     ngx_post_event(rev, queue);

            // } else {
            //     rev->handler(rev);
            // }
            if(sock_conn_fd != -11) {
                rev->handler(rev);
            }
            // else{
            //     printf("-11 accept!!\n");
            // }

            rev->ready = 0;
            rev->complete = 0;

            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_accept(sqe, c->fd, c->sockaddr, &c->socklen, 0);
            conn_info->conn = c;
            conn_info->rq_type = NGX_URING_ACCEPT;
            io_uring_sqe_set_data(sqe, conn_info);
            break;
        }
        /*case NGX_URING_WRITE:{
            wev->res = cqe->res;
            wev->ready = 1;
            wev->complete = 1;

            if (flags & NGX_POST_EVENTS) {
                ngx_post_event(wev, &ngx_posted_events);

            } else {
                wev->handler(wev);
            }
            return_uring_conn_info(conn_info);
            break;
        }*/
        case NGX_URING_READ:{
            rev->uring_res = cqe->res;
            rev->ready = 1;
            rev->complete = 1;
            rev->available = -1;

            if(cqe->res <= 0){
                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "io_uring error on fd:%d",
                           c->fd);
                rev->pending_eof = 1;
                //c->fd = -1;
            }
            //rev->handler(rev);
           
            if (flags & NGX_POST_EVENTS) {
                queue = rev->accept ? &ngx_posted_accept_events
                                    : &ngx_posted_events;

                ngx_post_event(rev, queue);

            } else {
                rev->handler(rev);
            }
            return_uring_conn_info(conn_info);
            break;
        }
        case NGX_URING_WRITEV:{
            //printf("WRITEV : %d\n", cqe->res);
            wev->rq_chain_cnt -= 1;
            wev->uring_res += cqe->res;
            
            //if(wev->rq_chain_cnt == 0){
            {
                //ngx_destroy_pool((ngx_pool_t*)conn_info->next);

                // ngx_pool_t  *pool;

                // c->destroyed = 1;

                // pool = c->pool;

                // ngx_close_connection(c);

                // ngx_destroy_pool(pool);
                //ngx_close_connection(c);


                // wev->ready = 1;
                // wev->complete = 0;
                // wev->pending = 0;
                // wev->uring_res = 0;

                // wev->complete = 0;
                // wev->pending = 0;
                // sent = wev->uring_res;
                // wev->uring_res = 0;
                // //printf("send_file_chain complete: %ld, %ld\n", wev->rq_size, sent);
                // if(sent != wev->rq_size){
                //     ngx_connection_error(c, 0, "uring_sendfile_chain failed");
                //     return NGX_ERROR;
                // }
                // ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                //    "uring_sendfile_chain complete: fd:%d, s:%ul", c->fd, sent);

                wev->complete = 1;
                wev->ready = 1;



                if (flags & NGX_POST_EVENTS) {
                    ngx_post_event(wev, &ngx_posted_events);

                } else {
                    wev->handler(wev);
                }
            }
            return_uring_conn_info(conn_info);
            break;
        }
        #if (0)
        case NGX_URING_SPLICE_IN:{
            //printf("SPLICE IN : %d\n", cqe->res);
            return_uring_conn_info(conn_info);
            break;
        }
        case NGX_URING_SPLICE_OUT:{

            //printf("SPLICE OUT : %d\n", cqe->res);
            wev->rq_chain_cnt -= 1;
            wev->uring_res += cqe->res;

            if(conn_info->splice_pipe)
                return_uring_splice_pipe(conn_info->splice_pipe);
            if(wev->rq_chain_cnt == 0){
                ngx_destroy_pool((ngx_pool_t*)conn_info->next);
                //ngx_pool_t  *pool;

                // c->destroyed = 1;

                // pool = c->pool;

                // ngx_close_connection(c);

                // ngx_destroy_pool(pool);
                //printf("send_chain compelete \n");
                // wev->complete = 1;
                wev->ready = 1;
                wev->complete = 0;
                wev->pending = 0;
                wev->uring_res = 0;

                // sent = wev->uring_res;
                
                // //printf("send_file_chain complete: %ld, %ld\n", wev->rq_size, sent);
                // if(sent != wev->rq_size){
                //     printf("send_chain error %ld, %ld\n", sent, wev->rq_size);
                //     ngx_connection_error(c, 0, "uring_sendfile_chain failed");
                //     return NGX_ERROR;
                // }
                // ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                //    "uring_sendfile_chain complete: fd:%d, s:%ul", c->fd, sent);

                // if (flags & NGX_POST_EVENTS) {
                //     ngx_post_event(wev, &ngx_posted_events);

                // } else {
                //     wev->handler(wev);
                // }
            }
            return_uring_conn_info(conn_info);
            break;
        }
        #endif
        case NGX_URING_NOTIFY_READ:{
            rev->uring_res = cqe->res;
            rev->ready = 1;
            rev->complete = 1;
            rev->available = -1;

            if(cqe->res <= 0){
                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "io_uring error on fd:%d",
                           c->fd);
                rev->pending_eof = 1;
                //c->fd = -1;
            }
           
            if (flags & NGX_POST_EVENTS) {
                queue = rev->accept ? &ngx_posted_accept_events
                                    : &ngx_posted_events;

                ngx_post_event(rev, queue);

            } else {
                rev->handler(rev);
            }
            break;
        }
        default:
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "io_uring invalid request type");
            return NGX_ERROR;
        }
        
        
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0, "sendfile: %z of %uz @%O",
                   n, size, file->file_pos);

    io_uring_cq_advance(&ring, count);
    return NGX_OK;
}


static void *
ngx_uring_create_conf(ngx_cycle_t *cycle)
{
    ngx_uring_conf_t  *epcf;

    epcf = ngx_palloc(cycle->pool, sizeof(ngx_uring_conf_t));
    if (epcf == NULL) {
        return NULL;
    }

    epcf->entries = NGX_CONF_UNSET;

    return epcf;
}


static char *
ngx_uring_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_uring_conf_t *epcf = conf;

    ngx_conf_init_uint_value(epcf->entries, 32768);

    return NGX_CONF_OK;
}


ssize_t
ngx_uring_recv(ngx_connection_t *c, u_char *buf, size_t size)
{
    //printf("ngx_uring_recv() %p\n", buf);
    ngx_event_t      *rev;
    int            nbytes;
    
    rev = c->read;

    if(rev->pending && !rev->complete) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0, "second uring_recv post");
        return NGX_AGAIN;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "rev->complete: %d", rev->complete);

    if(rev->complete) {

        rev->complete = 0;
        rev->pending = 0;
        nbytes = rev->uring_res;
        rev->uring_res = 0;
        rev->available = 0;
        //rev->ready = 0;
        //printf("recv complete: %ld, %d , %c, %c \n", size, nbytes, buf[2], buf[17]);

        if(nbytes == 0){
            rev->ready = 0;
            rev->eof = 1;
            return 0;
        }
        if(nbytes < 0){
            ngx_connection_error(c, 0, "uring_recv() failed");
            rev->error = 1;
            return NGX_ERROR;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "uring_recv: fd:%d %ul of %z",
                           c->fd, nbytes, size);

        return nbytes;
    }
    
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if(!sqe){
        rev->error = 1;
        ngx_connection_error(c, 0, "uring_writev() failed");
        return NGX_ERROR;
    }

    io_uring_prep_recv(sqe, c->fd, buf, size, 0);
    uring_conn_info* conn_info = get_uring_conn_info();
    conn_info->conn = c;
    conn_info->rq_type = NGX_URING_READ;
    io_uring_sqe_set_data(sqe, conn_info);

    rev->rq_size = size;
    rev->rq_chain_cnt = 1;
    rev->pending = 1;
    rev->complete = 0;
    rev->ready = 0;
    
    return NGX_AGAIN;
}

#if (0)
ngx_chain_t *
ngx_uring_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    //printf("sendfile_chain\n");
    u_char        *prev;
    off_t          send;
    size_t         file_size, sent;
    ngx_buf_t     *file;
    ngx_event_t   *wev;
    ngx_chain_t   *cl;
    ssize_t        n;
    ssize_t        size;
    int            nelts;
    int            rq_chain_cnt;
    int            start_el;
    ngx_pool_t    *pool;

    wev = c->write;
    pool = c->data;

    if(wev->pending && !wev->complete){
        return in;
    }
    
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "wev->complete: %d", wev->complete);
    
    if(wev->complete){
        wev->complete = 0;
        wev->pending = 0;
        sent = wev->uring_res;
        wev->uring_res = 0;
        printf("send_file_chain complete: %ld, %ld\n", wev->rq_size, sent);
        if(sent != wev->rq_size){
            ngx_connection_error(c, 0, "uring_sendfile_chain failed");
            return NGX_CHAIN_ERROR;
        }
        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "uring_sendfile_chain complete: fd:%d, s:%ul", c->fd, sent);
        c->sent += sent;
        in = ngx_chain_update_sent(in, sent);
        return in;
    }
   
    send = 0;
    nelts = 0;
    prev = NULL;
    rq_chain_cnt = 0;
    start_el = 0;

    if (limit == 0 || limit > (off_t) (NGX_SENDFILE_MAXSIZE - ngx_pagesize)) {
        limit = NGX_SENDFILE_MAXSIZE - ngx_pagesize;
    }

    for (cl = in;
         cl && nelts < NGX_IOVS_PREALLOCATE && send < limit;
        )
    {
        if (ngx_buf_special(cl->buf)) {
            cl = cl->next;
            continue;
        }

        if (cl->buf->in_file) {
            if(nelts > 0){

                ++rq_chain_cnt;
                n = ngx_uring_writev(c, nelts, start_el, pool);
                start_el += nelts;
                prev = NULL;

                if (n == NGX_ERROR) {
                    return NGX_CHAIN_ERROR;
                }
            }
            
            file = cl->buf;
            //printf("file fd : %d \n", cl->buf->file->fd);

            file_size = (size_t) ngx_chain_coalesce_file(&cl, limit - send);
            printf("file in chain!! %ld\n", file_size);

            send += file_size;

            if (file_size == 0) {
                ngx_debug_point();
                return NGX_CHAIN_ERROR;
            }
            
            n = ngx_uring_splice_sendfile(c, file, file_size, pool);   

            if (n == NGX_ERROR) {
                wev->error = 1;
                return NGX_CHAIN_ERROR;
            }
            
            //sent = (n == NGX_AGAIN) ? 0 : n;
            ++rq_chain_cnt;
            continue;
        }

        size = cl->buf->last - cl->buf->pos;
        if (send + size > limit) {
            size = (u_long) (limit - send);
        }

        if (prev == cl->buf->pos) {
            wev->iovecs[nelts - 1].iov_len += cl->buf->last - cl->buf->pos;

        } else {
            ++nelts;
            if (nelts >= NGX_IOVS_PREALLOCATE) {
                wev->error = 1;
                return NGX_CHAIN_ERROR;
            }
            wev->iovecs[nelts - 1].iov_base = (void *) cl->buf->pos;
            wev->iovecs[nelts - 1].iov_len = cl->buf->last - cl->buf->pos;
            //printf("send buf : %p \n", cl->buf->pos);
        }
        prev = cl->buf->last;
        send += size;
        cl = cl->next;
        //printf("iov.len : %ld   size : %ld, send : %ld \n",wev->iovecs[nelts - 1].iov_len, size, send );
    }

    
    int i, total = 0;
    for(i = 0; i < nelts; ++i){
        total += wev->iovecs[i].iov_len;
    }
    printf("nelts : %d  toal : %d  chain_cnt : %d, limit: %ld\n", nelts, total, rq_chain_cnt, limit);
    if(nelts - start_el > 0){
        ++rq_chain_cnt;
        n = ngx_uring_writev(c, nelts, start_el, pool);

        if (n == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        //sent = (n == NGX_AGAIN) ? 0 : n;
    }
    
    wev->rq_size = send;
    wev->rq_chain_cnt = rq_chain_cnt;
    wev->pending = 1;
    wev->complete = 0;
    //wev->ready = 0;
    wev->ready = 1;

    c->sent += send;
    in = ngx_chain_update_sent(in, send);
    io_uring_submit(&ring);
    //printf("send : %ld   in : %p\n", send, in);
    //printf("send_chain return ok \n");
    return in;
}



static ssize_t
ngx_uring_splice_sendfile(ngx_connection_t *c, ngx_buf_t *file, size_t size, void *pool_data)
{
    //printf("ngx_uring_splice_sendfile()\n");
    struct io_uring_sqe *sqe;
    uring_splice_pipe *p;
    uring_conn_info* conn_info;
#if (NGX_HAVE_SENDFILE64)
    off_t      offset;
#else
    int32_t    offset;
#endif
#if (NGX_HAVE_SENDFILE64)
    offset = file->file_pos;
#else
    offset = (int32_t) file->file_pos;
#endif

    sqe = io_uring_get_sqe(&ring);
    if(!sqe){
        c->write->error = 1;
        ngx_connection_error(c, 0, "uring_writev() failed");
        return NGX_ERROR;
    }

    p = get_uring_splice_pipe();
    //printf("splice pipes %d, %d\n", p->pipe_fds[0], p->pipe_fds[1]);

    io_uring_prep_splice(sqe, file->file->fd, offset
            , p->pipe_fds[1], -1, size,  SPLICE_F_MOVE | SPLICE_F_MORE);
    sqe->flags = IOSQE_IO_LINK;
    
    conn_info = get_uring_conn_info();
    conn_info->conn = c;
    conn_info->rq_type = NGX_URING_SPLICE_IN;
    io_uring_sqe_set_data(sqe, conn_info);

    
    sqe = io_uring_get_sqe(&ring);
    if(!sqe){
        c->write->error = 1;
        ngx_connection_error(c, 0, "uring_writev() failed");
        return NGX_ERROR;
    }
    io_uring_prep_splice(sqe, p->pipe_fds[0], -1, c->fd, -1, size, SPLICE_F_MOVE | SPLICE_F_MORE);
    conn_info = get_uring_conn_info();
    conn_info->conn = c;
    conn_info->rq_type = NGX_URING_SPLICE_OUT;
    conn_info->splice_pipe = p;
    conn_info->next = pool_data;
    io_uring_sqe_set_data(sqe, conn_info);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "uring_splice_sendfile: @%O %uz", file->file_pos, size);

    return NGX_AGAIN;
}
#endif

ssize_t
ngx_uring_writev(ngx_connection_t *c, int nelts, int start_el, void *pool_data)
{
    //printf("ngx_uring_writev()\n");
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if(!sqe){
        c->write->error = 1;
        ngx_connection_error(c, 0, "uring_writev() failed");
        return NGX_ERROR;
    }

    io_uring_prep_writev(sqe, c->fd, &c->write->iovecs[start_el], nelts - start_el , 0);

    uring_conn_info* conn_info = get_uring_conn_info();
    conn_info->conn = c;
    conn_info->rq_type = NGX_URING_WRITEV;
    conn_info->next = pool_data;

    io_uring_sqe_set_data(sqe, conn_info);

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "uring_writev() posted");
    return NGX_AGAIN;
}



ngx_chain_t *
ngx_uring_writev_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    u_char        *prev;
    ssize_t        n;
    size_t         sent;
    off_t          send;
    ngx_chain_t   *cl;
    ngx_event_t   *wev;
    ngx_pool_t    *pool;
    int            nelts;
    int            start_el;
    ssize_t        size;

    wev = c->write;
    pool = c->data;

    if(wev->pending && !wev->complete){
        return in;
    }
    if(wev->complete){
        wev->complete = 0;
        wev->pending = 0;
        sent = wev->uring_res;
        wev->uring_res = 0;
        if(sent != wev->rq_size){
            ngx_connection_error(c, 0, "uring_writev failed");
            return NGX_CHAIN_ERROR;
        }

        c->sent += sent;
        in = ngx_chain_update_sent(in, sent);
        return in;
    }

    send = 0;
    nelts = 0;
    prev = NULL;
    start_el = 0;


    /* the maximum limit size is the maximum size_t value - the page size */

    if (limit == 0 || limit > (off_t) (NGX_MAX_SIZE_T_VALUE - ngx_pagesize)) {
        limit = NGX_MAX_SIZE_T_VALUE - ngx_pagesize;
    }

    for (cl = in;
         cl && nelts < NGX_IOVS_PREALLOCATE && send < limit;
        )
    {
        
        if (ngx_buf_special(cl->buf)) {
            cl = cl->next;
            continue;
        }

        if (cl && cl->buf->in_file) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "file buf in writev "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);
            ngx_debug_point();

            return NGX_CHAIN_ERROR;
        }


        size = cl->buf->last - cl->buf->pos;
        if (send + size > limit) {
            size = (u_long) (limit - send);
        }

        if (prev == cl->buf->pos) {
            wev->iovecs[nelts - 1].iov_len += cl->buf->last - cl->buf->pos;

        } else {
            ++nelts;
            if (nelts >= NGX_IOVS_PREALLOCATE) {
                wev->error = 1;
                return NGX_CHAIN_ERROR;
            }
            wev->iovecs[nelts - 1].iov_base = (void *) cl->buf->pos;
            wev->iovecs[nelts - 1].iov_len = cl->buf->last - cl->buf->pos;
            //printf("send buf : %p \n", cl->buf->pos);
        }
        prev = cl->buf->last;
        send += size;
        cl = cl->next;
        //printf("iov.len : %ld   size : %ld, send : %ld \n",wev->iovecs[nelts - 1].iov_len, size, send );
    }


    if(nelts - start_el > 0){
        n = ngx_uring_writev(c, nelts, start_el, pool);

        if (n == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        //sent = (n == NGX_AGAIN) ? 0 : n;
    }
    
    wev->rq_size = send;
    wev->rq_chain_cnt = 1;
    wev->pending = 1;
    wev->complete = 0;
    wev->ready = 0;
    //wev->ready = 1;

    //c->sent += send;
    //in = ngx_chain_update_sent(in, send);
    //printf("send : %ld   in : %p\n", send, in);
    //printf("send_chain return ok \n");
    return in;
}



uring_conn_info *get_uring_conn_info()
{
    uring_conn_info *ret = uring_conn_infos;
    if(ret){
        uring_conn_infos = uring_conn_infos->next;
    } else{
        ret = (uring_conn_info*)malloc(sizeof(uring_conn_info));
        ret->conn = NULL;
        ret->splice_pipe = NULL;
        //printf("conn_info empty!!\n");
    }
    ret->next = NULL;
    return ret;
}

void return_uring_conn_info(uring_conn_info* conn_info)
{
    conn_info->next = uring_conn_infos;
    conn_info->conn = NULL;
    conn_info->splice_pipe = NULL;
    uring_conn_infos = conn_info;
}

#if (0)
uring_splice_pipe *get_uring_splice_pipe()
{
    uring_splice_pipe *ret = uring_splice_pipes;
    if(ret){
        uring_splice_pipes = uring_splice_pipes->next;
    } else{
        ret = (uring_splice_pipe*)malloc(sizeof(uring_splice_pipe));
        //printf("pipe empty!!\n");
        if(pipe(ret->pipe_fds) < 0) return NULL;
    }
    ret->next = NULL;
    return ret;
}

void return_uring_splice_pipe(uring_splice_pipe* p)
{
    p->next = uring_splice_pipes;
    uring_splice_pipes = p;
}
#endif