
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include "liburing.h"

ngx_uint_t                  ngx_use_epoll_rdhup = 1;

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

static uring_splice_pipe *uring_splice_pipes;
static uring_splice_pipe *get_uring_splice_pipe();
static void return_uring_splice_pipe(uring_splice_pipe* p);



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
static ngx_chain_t *ngx_uring_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit);
static ssize_t ngx_uring_writev(ngx_connection_t *c, int nelts, int start_el);
static ssize_t ngx_uring_splice_sendfile(ngx_connection_t *c, ngx_buf_t *file, size_t size);


static struct io_uring ring;


// #if (NGX_HAVE_EVENTFD)
// static int                  notify_fd = -1;
// static ngx_event_t          notify_event;
// static ngx_connection_t     notify_conn;
// static uint64_t             notify_count_buf;
// static uring_conn_info      notify_conn_info;
// #endif

static ngx_str_t      uring_name = ngx_string("epoll");

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
ngx_module_t  ngx_epoll_module = {
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
    ngx_uring_sendfile_chain,
    NGX_IO_SENDFILE
};




static ngx_int_t
ngx_uring_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    //ngx_write_console(ngx_stderr, "ngx_uring_init()\n", strlen("ngx_uring_init()\n"));
    ngx_uring_conf_t  *epcf;
    long unsigned int i;

    epcf = ngx_event_get_conf(cycle->conf_ctx, ngx_epoll_module);

    
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

    ngx_io = ngx_uring_io;

    ngx_event_actions = ngx_uring_module_ctx.actions;

#if (NGX_HAVE_CLEAR_EVENT)
    ngx_event_flags = NGX_USE_CLEAR_EVENT
#else
    ngx_event_flags = NGX_USE_LEVEL_EVENT
#endif
                      |NGX_USE_GREEDY_EVENT
                      |NGX_USE_EPOLL_EVENT;
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
            wev->rq_chain_cnt -= 1;
            wev->uring_res += cqe->res;
            if(wev->rq_chain_cnt == 0){
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

    ngx_conf_init_uint_value(epcf->entries, 1024);

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
    
    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                           "uring_recv() posted");
    return NGX_AGAIN;
}


ngx_chain_t *
ngx_uring_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    //printf("ngx_uring_sendfile_chain()\n");
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

    wev = c->write;

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
        //printf("send_file_chain complete: %ld, %ld\n", wev->rq_size, sent);
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
                //printf("wriev first!!\n");
                n = ngx_uring_writev(c, nelts, start_el);
                start_el += nelts;
                prev = NULL;

                if (n == NGX_ERROR) {
                    return NGX_CHAIN_ERROR;
                }
            }
            
            file = cl->buf;

            file_size = (size_t) ngx_chain_coalesce_file(&cl, limit - send);
            //printf("file in chain!! %ld\n", file_size);

            send += file_size;

            if (file_size == 0) {
                ngx_debug_point();
                wev->error = 1;
                return NGX_CHAIN_ERROR;
            }
            
            n = ngx_uring_splice_sendfile(c, file, file_size);   

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
        }
        prev = cl->buf->last;
        send += size;
        cl = cl->next;
        //printf("iov.len : %ld   size : %ld, send : %ld \n",wev->iovecs[nelts - 1].iov_len, size, send );
    }

    
    // int i, total = 0;
    // for(i = 0; i < nelts; ++i){
    //     total += wev->iovecs[i].iov_len;
    // }
    // printf("nelts : %d  toal : %d  chain_cnt : %d, limit: %ld\n", nelts, total, rq_chain_cnt, limit);
    if(nelts - start_el > 0){
        ++rq_chain_cnt;
        n = ngx_uring_writev(c, nelts, start_el);

        if (n == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        //sent = (n == NGX_AGAIN) ? 0 : n;
    }
    
    wev->rq_size = send;
    wev->rq_chain_cnt = rq_chain_cnt;
    wev->pending = 1;
    wev->complete = 0;
    wev->ready = 0;
    return in;
}

ssize_t
ngx_uring_writev(ngx_connection_t *c, int nelts, int start_el)
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

    io_uring_sqe_set_data(sqe, conn_info);

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "uring_writev() posted");
    return NGX_AGAIN;
}



static ssize_t
ngx_uring_splice_sendfile(ngx_connection_t *c, ngx_buf_t *file, size_t size)
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
    io_uring_sqe_set_data(sqe, conn_info);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "uring_splice_sendfile: @%O %uz", file->file_pos, size);

    return NGX_AGAIN;
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

uring_splice_pipe *get_uring_splice_pipe()
{
    uring_splice_pipe *ret = uring_splice_pipes;
    if(ret){
        uring_splice_pipes = uring_splice_pipes->next;
    } else{
        ret = (uring_splice_pipe*)malloc(sizeof(uring_splice_pipe));
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


/*

ssize_t
ngx_uring_readv_chain(ngx_connection_t *c, ngx_chain_t *chain, off_t limit)
{
    u_char        *prev;
    ssize_t        n, size;
    ngx_err_t      err;
    ngx_event_t   *rev;

    rev = c->read;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "uring_readv: eof:%d, avail:%d",
                       rev->pending_eof, rev->available);

    if (rev->available == 0) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0, "second uring_readv post");
        return NGX_AGAIN;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "rev->complete: %d", rev->complete);

    if(rev->complete) {
        rev->complete = 0;
        rev->ready = 0;
        if(rev->res == 0){
            rev->ready = 0;
            rev->eof = 1;
            return 0;
        }
        if(rev->res < 0){
            ngx_connection_error(c, 0, "uring_readv() failed");
            rev->error = 1;
            return NGX_ERROR;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "uring_readv: %ui, last:%uz", rev->nelts, rev->iovs[rev->nelts].iov_len);

        return rev->res;
    }


    prev = NULL;
    size = 0;

    int nelts = 0;
    

    // coalesce the neighbouring bufs 

    while (chain) {
        n = chain->buf->end - chain->buf->last;

        if (limit) {
            if (size >= limit) {
                break;
            }

            if (size + n > limit) {
                n = (ssize_t) (limit - size);
            }
        }

        if (prev == chain->buf->last) {
            rev->iovs[nelts].iov_len += n;

        } else {
            ++nelts;
            if (nelts >= NGX_IOVS_PREALLOCATE) {
                rev->error = 1;
                return NGX_CHAIN_ERROR;
            }

            

            rev->iovs[nelts].iov_base = (void *) chain->buf->last;
            rev->iovs[nelts].iov_len = n;
        }

        size += n;
        prev = chain->buf->end;
        chain = chain->next;
    }

    rev->nelts = nelts;
    
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_readv(sqe, c->fd, rev->iovs, nelts, 0);
    c->conn_info.conn = c;
    c->conn_info.rq_type = NGX_URING_READ;
    io_uring_sqe_set_data(sqe, &(c->conn_info));

    rev->active = 1;
    rev->complete = 0;
    rev->available = 0;
    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                           "uring_readv() posted");
    return NGX_AGAIN;
}
*/









// /*
//  * Copyright (C) Igor Sysoev
//  * Copyright (C) Nginx, Inc.
//  */


// #include <ngx_config.h>
// #include <ngx_core.h>
// #include <ngx_event.h>


// #if (NGX_TEST_BUILD_EPOLL)

// /* epoll declarations */

// #define EPOLLIN        0x001
// #define EPOLLPRI       0x002
// #define EPOLLOUT       0x004
// #define EPOLLERR       0x008
// #define EPOLLHUP       0x010
// #define EPOLLRDNORM    0x040
// #define EPOLLRDBAND    0x080
// #define EPOLLWRNORM    0x100
// #define EPOLLWRBAND    0x200
// #define EPOLLMSG       0x400

// #define EPOLLRDHUP     0x2000

// #define EPOLLEXCLUSIVE 0x10000000
// #define EPOLLONESHOT   0x40000000
// #define EPOLLET        0x80000000

// #define EPOLL_CTL_ADD  1
// #define EPOLL_CTL_DEL  2
// #define EPOLL_CTL_MOD  3

// typedef union epoll_data {
//     void         *ptr;
//     int           fd;
//     uint32_t      u32;
//     uint64_t      u64;
// } epoll_data_t;

// struct epoll_event {
//     uint32_t      events;
//     epoll_data_t  data;
// };


// int epoll_create(int size);

// int epoll_create(int size)
// {
//     return -1;
// }


// int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);

// int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
// {
//     return -1;
// }


// int epoll_wait(int epfd, struct epoll_event *events, int nevents, int timeout);

// int epoll_wait(int epfd, struct epoll_event *events, int nevents, int timeout)
// {
//     return -1;
// }

// #if (NGX_HAVE_EVENTFD)
// #define SYS_eventfd       323
// #endif

// #if (NGX_HAVE_FILE_AIO)

// #define SYS_io_setup      245
// #define SYS_io_destroy    246
// #define SYS_io_getevents  247

// typedef u_int  aio_context_t;

// struct io_event {
//     uint64_t  data;  /* the data field from the iocb */
//     uint64_t  obj;   /* what iocb this event came from */
//     int64_t   res;   /* result code for this event */
//     int64_t   res2;  /* secondary result */
// };


// #endif
// #endif /* NGX_TEST_BUILD_EPOLL */


// typedef struct {
//     ngx_uint_t  events;
//     ngx_uint_t  aio_requests;
// } ngx_epoll_conf_t;


// static ngx_int_t ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer);
// // #if (NGX_HAVE_EVENTFD)
// // static ngx_int_t ngx_epoll_notify_init(ngx_log_t *log);
// // static void ngx_epoll_notify_handler(ngx_event_t *ev);
// // #endif
// #if (NGX_HAVE_EPOLLRDHUP)
// static void ngx_epoll_test_rdhup(ngx_cycle_t *cycle);
// #endif
// static void ngx_epoll_done(ngx_cycle_t *cycle);
// static ngx_int_t ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event,
//     ngx_uint_t flags);
// static ngx_int_t ngx_epoll_del_event(ngx_event_t *ev, ngx_int_t event,
//     ngx_uint_t flags);
// static ngx_int_t ngx_epoll_add_connection(ngx_connection_t *c);
// static ngx_int_t ngx_epoll_del_connection(ngx_connection_t *c,
//     ngx_uint_t flags);
// // #if (NGX_HAVE_EVENTFD)
// // static ngx_int_t ngx_epoll_notify(ngx_event_handler_pt handler);
// // #endif
// static ngx_int_t ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
//     ngx_uint_t flags);

// #if (NGX_HAVE_FILE_AIO)
// static void ngx_epoll_eventfd_handler(ngx_event_t *ev);
// #endif

// static void *ngx_epoll_create_conf(ngx_cycle_t *cycle);
// static char *ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf);

// static int                  ep = -1;
// static struct epoll_event  *event_list;
// static ngx_uint_t           nevents;

// // #if (NGX_HAVE_EVENTFD)
// // static int                  notify_fd = -1;
// // static ngx_event_t          notify_event;
// // static ngx_connection_t     notify_conn;
// // #endif

// #if (NGX_HAVE_FILE_AIO)

// int                         ngx_eventfd = -1;
// aio_context_t               ngx_aio_ctx = 0;

// static ngx_event_t          ngx_eventfd_event;
// static ngx_connection_t     ngx_eventfd_conn;

// #endif

// #if (NGX_HAVE_EPOLLRDHUP)
// ngx_uint_t                  ngx_use_epoll_rdhup;
// #endif

// static ngx_str_t      epoll_name = ngx_string("epoll");

// static ngx_command_t  ngx_epoll_commands[] = {

//     { ngx_string("epoll_events"),
//       NGX_EVENT_CONF|NGX_CONF_TAKE1,
//       ngx_conf_set_num_slot,
//       0,
//       offsetof(ngx_epoll_conf_t, events),
//       NULL },

//     { ngx_string("worker_aio_requests"),
//       NGX_EVENT_CONF|NGX_CONF_TAKE1,
//       ngx_conf_set_num_slot,
//       0,
//       offsetof(ngx_epoll_conf_t, aio_requests),
//       NULL },

//       ngx_null_command
// };


// static ngx_event_module_t  ngx_epoll_module_ctx = {
//     &epoll_name,
//     ngx_epoll_create_conf,               /* create configuration */
//     ngx_epoll_init_conf,                 /* init configuration */

//     {
//         ngx_epoll_add_event,             /* add an event */
//         ngx_epoll_del_event,             /* delete an event */
//         ngx_epoll_add_event,             /* enable an event */
//         ngx_epoll_del_event,             /* disable an event */
//         ngx_epoll_add_connection,        /* add an connection */
//         ngx_epoll_del_connection,        /* delete an connection */
// // #if (NGX_HAVE_EVENTFD)
// //         ngx_epoll_notify,                /* trigger a notify */
// // #else
//         NULL,                            /* trigger a notify */
// //#endif
//         ngx_epoll_process_events,        /* process the events */
//         ngx_epoll_init,                  /* init the events */
//         ngx_epoll_done,                  /* done the events */
//     }
// };

// ngx_module_t  ngx_epoll_module = {
//     NGX_MODULE_V1,
//     &ngx_epoll_module_ctx,               /* module context */
//     ngx_epoll_commands,                  /* module directives */
//     NGX_EVENT_MODULE,                    /* module type */
//     NULL,                                /* init master */
//     NULL,                                /* init module */
//     NULL,                                /* init process */
//     NULL,                                /* init thread */
//     NULL,                                /* exit thread */
//     NULL,                                /* exit process */
//     NULL,                                /* exit master */
//     NGX_MODULE_V1_PADDING
// };


// #if (NGX_HAVE_FILE_AIO)

// /*
//  * We call io_setup(), io_destroy() io_submit(), and io_getevents() directly
//  * as syscalls instead of libaio usage, because the library header file
//  * supports eventfd() since 0.3.107 version only.
//  */

// static int
// io_setup(u_int nr_reqs, aio_context_t *ctx)
// {
//     return syscall(SYS_io_setup, nr_reqs, ctx);
// }


// static int
// io_destroy(aio_context_t ctx)
// {
//     return syscall(SYS_io_destroy, ctx);
// }


// static int
// io_getevents(aio_context_t ctx, long min_nr, long nr, struct io_event *events,
//     struct timespec *tmo)
// {
//     return syscall(SYS_io_getevents, ctx, min_nr, nr, events, tmo);
// }


// static void
// ngx_epoll_aio_init(ngx_cycle_t *cycle, ngx_epoll_conf_t *epcf)
// {
//     int                 n;
//     struct epoll_event  ee;

// #if (NGX_HAVE_SYS_EVENTFD_H)
//     ngx_eventfd = eventfd(0, 0);
// #else
//     ngx_eventfd = syscall(SYS_eventfd, 0);
// #endif

//     if (ngx_eventfd == -1) {
//         ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
//                       "eventfd() failed");
//         ngx_file_aio = 0;
//         return;
//     }

//     ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
//                    "eventfd: %d", ngx_eventfd);

//     n = 1;

//     if (ioctl(ngx_eventfd, FIONBIO, &n) == -1) {
//         ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
//                       "ioctl(eventfd, FIONBIO) failed");
//         goto failed;
//     }

//     if (io_setup(epcf->aio_requests, &ngx_aio_ctx) == -1) {
//         ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
//                       "io_setup() failed");
//         goto failed;
//     }

//     ngx_eventfd_event.data = &ngx_eventfd_conn;
//     ngx_eventfd_event.handler = ngx_epoll_eventfd_handler;
//     ngx_eventfd_event.log = cycle->log;
//     ngx_eventfd_event.active = 1;
//     ngx_eventfd_conn.fd = ngx_eventfd;
//     ngx_eventfd_conn.read = &ngx_eventfd_event;
//     ngx_eventfd_conn.log = cycle->log;

//     ee.events = EPOLLIN|EPOLLET;
//     ee.data.ptr = &ngx_eventfd_conn;

//     if (epoll_ctl(ep, EPOLL_CTL_ADD, ngx_eventfd, &ee) != -1) {
//         return;
//     }

//     ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
//                   "epoll_ctl(EPOLL_CTL_ADD, eventfd) failed");

//     if (io_destroy(ngx_aio_ctx) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "io_destroy() failed");
//     }

// failed:

//     if (close(ngx_eventfd) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "eventfd close() failed");
//     }

//     ngx_eventfd = -1;
//     ngx_aio_ctx = 0;
//     ngx_file_aio = 0;
// }

// #endif


// static ngx_int_t
// ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
// {
//     //(void) ngx_write_console(ngx_stderr, "ngx_epoll_init()\n", strlen("ngx_epoll_init()\n"));
//     ngx_epoll_conf_t  *epcf;

//     epcf = ngx_event_get_conf(cycle->conf_ctx, ngx_epoll_module);

//     if (ep == -1) {
//         ep = epoll_create(cycle->connection_n / 2);

//         if (ep == -1) {
//             ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
//                           "epoll_create() failed");
//             return NGX_ERROR;
//         }

// // #if (NGX_HAVE_EVENTFD)
// //         if (ngx_epoll_notify_init(cycle->log) != NGX_OK) {
// //             ngx_epoll_module_ctx.actions.notify = NULL;
// //         }
// // #endif

// #if (NGX_HAVE_FILE_AIO)
//         ngx_epoll_aio_init(cycle, epcf);
// #endif

// #if (NGX_HAVE_EPOLLRDHUP)
//         ngx_epoll_test_rdhup(cycle);
// #endif
//     }

//     if (nevents < epcf->events) {
//         if (event_list) {
//             ngx_free(event_list);
//         }

//         event_list = ngx_alloc(sizeof(struct epoll_event) * epcf->events,
//                                cycle->log);
//         if (event_list == NULL) {
//             return NGX_ERROR;
//         }
//     }

//     nevents = epcf->events;

//     ngx_io = ngx_os_io;

//     ngx_event_actions = ngx_epoll_module_ctx.actions;

// #if (NGX_HAVE_CLEAR_EVENT)
//     ngx_event_flags = NGX_USE_CLEAR_EVENT
// #else
//     ngx_event_flags = NGX_USE_LEVEL_EVENT
// #endif
//                       |NGX_USE_GREEDY_EVENT
//                       |NGX_USE_EPOLL_EVENT;
//     //ngx_use_epoll_rdhup = 0;

//     return NGX_OK;
// }


// // #if (NGX_HAVE_EVENTFD)

// // static ngx_int_t
// // ngx_epoll_notify_init(ngx_log_t *log)
// // {
// //     struct epoll_event  ee;

// // #if (NGX_HAVE_SYS_EVENTFD_H)
// //     notify_fd = eventfd(0, 0);
// // #else
// //     notify_fd = syscall(SYS_eventfd, 0);
// // #endif

// //     if (notify_fd == -1) {
// //         ngx_log_error(NGX_LOG_EMERG, log, ngx_errno, "eventfd() failed");
// //         return NGX_ERROR;
// //     }

// //     ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0,
// //                    "notify eventfd: %d", notify_fd);

// //     notify_event.handler = ngx_epoll_notify_handler;
// //     notify_event.log = log;
// //     notify_event.active = 1;

// //     notify_conn.fd = notify_fd;
// //     notify_conn.read = &notify_event;
// //     notify_conn.log = log;

// //     ee.events = EPOLLIN|EPOLLET;
// //     ee.data.ptr = &notify_conn;

// //     if (epoll_ctl(ep, EPOLL_CTL_ADD, notify_fd, &ee) == -1) {
// //         ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
// //                       "epoll_ctl(EPOLL_CTL_ADD, eventfd) failed");

// //         if (close(notify_fd) == -1) {
// //             ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
// //                             "eventfd close() failed");
// //         }

// //         return NGX_ERROR;
// //     }

// //     return NGX_OK;
// // }


// // static void
// // ngx_epoll_notify_handler(ngx_event_t *ev)
// // {
// //     ssize_t               n;
// //     uint64_t              count;
// //     ngx_err_t             err;
// //     ngx_event_handler_pt  handler;

// //     if (++ev->index == NGX_MAX_UINT32_VALUE) {
// //         ev->index = 0;

// //         n = read(notify_fd, &count, sizeof(uint64_t));

// //         err = ngx_errno;

// //         ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
// //                        "read() eventfd %d: %z count:%uL", notify_fd, n, count);

// //         if ((size_t) n != sizeof(uint64_t)) {
// //             ngx_log_error(NGX_LOG_ALERT, ev->log, err,
// //                           "read() eventfd %d failed", notify_fd);
// //         }
// //     }

// //     handler = ev->data;
// //     handler(ev);
// // }

// // #endif


// #if (NGX_HAVE_EPOLLRDHUP)

// static void
// ngx_epoll_test_rdhup(ngx_cycle_t *cycle)
// {
//     int                 s[2], events;
//     struct epoll_event  ee;

//     if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "socketpair() failed");
//         return;
//     }

//     ee.events = EPOLLET|EPOLLIN|EPOLLRDHUP;

//     if (epoll_ctl(ep, EPOLL_CTL_ADD, s[0], &ee) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "epoll_ctl() failed");
//         goto failed;
//     }

//     if (close(s[1]) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "close() failed");
//         s[1] = -1;
//         goto failed;
//     }

//     s[1] = -1;

//     events = epoll_wait(ep, &ee, 1, 5000);

//     if (events == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "epoll_wait() failed");
//         goto failed;
//     }

//     if (events) {
//         ngx_use_epoll_rdhup = ee.events & EPOLLRDHUP;

//     } else {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, NGX_ETIMEDOUT,
//                       "epoll_wait() timed out");
//     }

//     ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
//                    "testing the EPOLLRDHUP flag: %s",
//                    ngx_use_epoll_rdhup ? "success" : "fail");

// failed:

//     if (s[1] != -1 && close(s[1]) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "close() failed");
//     }

//     if (close(s[0]) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "close() failed");
//     }
// }

// #endif


// static void
// ngx_epoll_done(ngx_cycle_t *cycle)
// {
//     if (close(ep) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                       "epoll close() failed");
//     }

//     ep = -1;

// // #if (NGX_HAVE_EVENTFD)

// //     if (close(notify_fd) == -1) {
// //         ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
// //                       "eventfd close() failed");
// //     }

// //     notify_fd = -1;

// // #endif

// #if (NGX_HAVE_FILE_AIO)

//     if (ngx_eventfd != -1) {

//         if (io_destroy(ngx_aio_ctx) == -1) {
//             ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                           "io_destroy() failed");
//         }

//         if (close(ngx_eventfd) == -1) {
//             ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
//                           "eventfd close() failed");
//         }

//         ngx_eventfd = -1;
//     }

//     ngx_aio_ctx = 0;

// #endif

//     ngx_free(event_list);

//     event_list = NULL;
//     nevents = 0;
// }


// static ngx_int_t
// ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
// {
//     int                  op;
//     uint32_t             events, prev;
//     ngx_event_t         *e;
//     ngx_connection_t    *c;
//     struct epoll_event   ee;

//     c = ev->data;

//     events = (uint32_t) event;

//     if (event == NGX_READ_EVENT) {
//         e = c->write;
//         prev = EPOLLOUT;
// #if (NGX_READ_EVENT != EPOLLIN|EPOLLRDHUP)
//         events = EPOLLIN|EPOLLRDHUP;
// #endif

//     } else {
//         e = c->read;
//         prev = EPOLLIN|EPOLLRDHUP;
// #if (NGX_WRITE_EVENT != EPOLLOUT)
//         events = EPOLLOUT;
// #endif
//     }

//     if (e->active) {
//         op = EPOLL_CTL_MOD;
//         events |= prev;

//     } else {
//         op = EPOLL_CTL_ADD;
//     }

// #if (NGX_HAVE_EPOLLEXCLUSIVE && NGX_HAVE_EPOLLRDHUP)
//     if (flags & NGX_EXCLUSIVE_EVENT) {
//         events &= ~EPOLLRDHUP;
//     }
// #endif

//     ee.events = events | (uint32_t) flags;
//     ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);

//     ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
//                    "epoll add event: fd:%d op:%d ev:%08XD",
//                    c->fd, op, ee.events);

//     if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
//                       "epoll_ctl(%d, %d) failed", op, c->fd);
//         return NGX_ERROR;
//     }

//     ev->active = 1;
// #if 0
//     ev->oneshot = (flags & NGX_ONESHOT_EVENT) ? 1 : 0;
// #endif

//     return NGX_OK;
// }


// static ngx_int_t
// ngx_epoll_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
// {
//     int                  op;
//     uint32_t             prev;
//     ngx_event_t         *e;
//     ngx_connection_t    *c;
//     struct epoll_event   ee;

//     /*
//      * when the file descriptor is closed, the epoll automatically deletes
//      * it from its queue, so we do not need to delete explicitly the event
//      * before the closing the file descriptor
//      */

//     if (flags & NGX_CLOSE_EVENT) {
//         ev->active = 0;
//         return NGX_OK;
//     }

//     c = ev->data;

//     if (event == NGX_READ_EVENT) {
//         e = c->write;
//         prev = EPOLLOUT;

//     } else {
//         e = c->read;
//         prev = EPOLLIN|EPOLLRDHUP;
//     }

//     if (e->active) {
//         op = EPOLL_CTL_MOD;
//         ee.events = prev | (uint32_t) flags;
//         ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);

//     } else {
//         op = EPOLL_CTL_DEL;
//         ee.events = 0;
//         ee.data.ptr = NULL;
//     }

//     ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
//                    "epoll del event: fd:%d op:%d ev:%08XD",
//                    c->fd, op, ee.events);

//     if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
//                       "epoll_ctl(%d, %d) failed", op, c->fd);
//         return NGX_ERROR;
//     }

//     ev->active = 0;

//     return NGX_OK;
// }


// static ngx_int_t
// ngx_epoll_add_connection(ngx_connection_t *c)
// {
//     struct epoll_event  ee;

//     ee.events = EPOLLIN|EPOLLOUT|EPOLLET|EPOLLRDHUP;
//     ee.data.ptr = (void *) ((uintptr_t) c | c->read->instance);

//     ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
//                    "epoll add connection: fd:%d ev:%08XD", c->fd, ee.events);

//     if (epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ee) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
//                       "epoll_ctl(EPOLL_CTL_ADD, %d) failed", c->fd);
//         return NGX_ERROR;
//     }

//     c->read->active = 1;
//     c->write->active = 1;

//     return NGX_OK;
// }


// static ngx_int_t
// ngx_epoll_del_connection(ngx_connection_t *c, ngx_uint_t flags)
// {
//     int                 op;
//     struct epoll_event  ee;

//     /*
//      * when the file descriptor is closed the epoll automatically deletes
//      * it from its queue so we do not need to delete explicitly the event
//      * before the closing the file descriptor
//      */

//     if (flags & NGX_CLOSE_EVENT) {
//         c->read->active = 0;
//         c->write->active = 0;
//         return NGX_OK;
//     }

//     ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
//                    "epoll del connection: fd:%d", c->fd);

//     op = EPOLL_CTL_DEL;
//     ee.events = 0;
//     ee.data.ptr = NULL;

//     if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
//         ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
//                       "epoll_ctl(%d, %d) failed", op, c->fd);
//         return NGX_ERROR;
//     }

//     c->read->active = 0;
//     c->write->active = 0;

//     return NGX_OK;
// }


// // #if (NGX_HAVE_EVENTFD)

// // static ngx_int_t
// // ngx_epoll_notify(ngx_event_handler_pt handler)
// // {
// //     static uint64_t inc = 1;

// //     notify_event.data = handler;

// //     if ((size_t) write(notify_fd, &inc, sizeof(uint64_t)) != sizeof(uint64_t)) {
// //         ngx_log_error(NGX_LOG_ALERT, notify_event.log, ngx_errno,
// //                       "write() to eventfd %d failed", notify_fd);
// //         return NGX_ERROR;
// //     }

// //     return NGX_OK;
// // }

// // #endif


// static ngx_int_t
// ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
// {
//     int                events;
//     uint32_t           revents;
//     ngx_int_t          instance, i;
//     ngx_uint_t         level;
//     ngx_err_t          err;
//     ngx_event_t       *rev, *wev;
//     ngx_queue_t       *queue;
//     ngx_connection_t  *c;

//     /* NGX_TIMER_INFINITE == INFTIM */

//     ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
//                    "epoll timer: %M", timer);

//     events = epoll_wait(ep, event_list, (int) nevents, timer);

//     err = (events == -1) ? ngx_errno : 0;

//     if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm) {
//         ngx_time_update();
//     }

//     if (err) {
//         if (err == NGX_EINTR) {

//             if (ngx_event_timer_alarm) {
//                 ngx_event_timer_alarm = 0;
//                 return NGX_OK;
//             }

//             level = NGX_LOG_INFO;

//         } else {
//             level = NGX_LOG_ALERT;
//         }

//         ngx_log_error(level, cycle->log, err, "epoll_wait() failed");
//         return NGX_ERROR;
//     }

//     if (events == 0) {
//         if (timer != NGX_TIMER_INFINITE) {
//             return NGX_OK;
//         }

//         ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
//                       "epoll_wait() returned no events without timeout");
//         return NGX_ERROR;
//     }

//     for (i = 0; i < events; i++) {
//         c = event_list[i].data.ptr;

//         instance = (uintptr_t) c & 1;
//         c = (ngx_connection_t *) ((uintptr_t) c & (uintptr_t) ~1);

//         rev = c->read;

//         if (c->fd == -1 || rev->instance != instance) {

//             /*
//              * the stale event from a file descriptor
//              * that was just closed in this iteration
//              */

//             ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
//                            "epoll: stale event %p", c);
//             continue;
//         }

//         revents = event_list[i].events;

//         ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
//                        "epoll: fd:%d ev:%04XD d:%p",
//                        c->fd, revents, event_list[i].data.ptr);

//         if (revents & (EPOLLERR|EPOLLHUP)) {
//             ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
//                            "epoll_wait() error on fd:%d ev:%04XD",
//                            c->fd, revents);

//             /*
//              * if the error events were returned, add EPOLLIN and EPOLLOUT
//              * to handle the events at least in one active handler
//              */

//             revents |= EPOLLIN|EPOLLOUT;
//         }

// #if 0
//         if (revents & ~(EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {
//             ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
//                           "strange epoll_wait() events fd:%d ev:%04XD",
//                           c->fd, revents);
//         }
// #endif

//         if ((revents & EPOLLIN) && rev->active) {

// #if (NGX_HAVE_EPOLLRDHUP)
//             if (revents & EPOLLRDHUP) {
//                 rev->pending_eof = 1;
//             }
// #endif

//             rev->ready = 1;
//             rev->available = -1;

//             if (flags & NGX_POST_EVENTS) {
//                 queue = rev->accept ? &ngx_posted_accept_events
//                                     : &ngx_posted_events;

//                 ngx_post_event(rev, queue);

//             } else {
//                 rev->handler(rev);
//             }
//         }

//         wev = c->write;

//         if ((revents & EPOLLOUT) && wev->active) {

//             if (c->fd == -1 || wev->instance != instance) {

//                 /*
//                  * the stale event from a file descriptor
//                  * that was just closed in this iteration
//                  */

//                 ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
//                                "epoll: stale event %p", c);
//                 continue;
//             }

//             wev->ready = 1;
// #if (NGX_THREADS)
//             wev->complete = 1;
// #endif

//             if (flags & NGX_POST_EVENTS) {
//                 ngx_post_event(wev, &ngx_posted_events);

//             } else {
//                 wev->handler(wev);
//             }
//         }
//     }

//     return NGX_OK;
// }


// #if (NGX_HAVE_FILE_AIO)

// static void
// ngx_epoll_eventfd_handler(ngx_event_t *ev)
// {
//     int               n, events;
//     long              i;
//     uint64_t          ready;
//     ngx_err_t         err;
//     ngx_event_t      *e;
//     ngx_event_aio_t  *aio;
//     struct io_event   event[64];
//     struct timespec   ts;

//     ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "eventfd handler");

//     n = read(ngx_eventfd, &ready, 8);

//     err = ngx_errno;

//     ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "eventfd: %d", n);

//     if (n != 8) {
//         if (n == -1) {
//             if (err == NGX_EAGAIN) {
//                 return;
//             }

//             ngx_log_error(NGX_LOG_ALERT, ev->log, err, "read(eventfd) failed");
//             return;
//         }

//         ngx_log_error(NGX_LOG_ALERT, ev->log, 0,
//                       "read(eventfd) returned only %d bytes", n);
//         return;
//     }

//     ts.tv_sec = 0;
//     ts.tv_nsec = 0;

//     while (ready) {

//         events = io_getevents(ngx_aio_ctx, 1, 64, event, &ts);

//         ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,
//                        "io_getevents: %d", events);

//         if (events > 0) {
//             ready -= events;

//             for (i = 0; i < events; i++) {

//                 ngx_log_debug4(NGX_LOG_DEBUG_EVENT, ev->log, 0,
//                                "io_event: %XL %XL %L %L",
//                                 event[i].data, event[i].obj,
//                                 event[i].res, event[i].res2);

//                 e = (ngx_event_t *) (uintptr_t) event[i].data;

//                 e->complete = 1;
//                 e->active = 0;
//                 e->ready = 1;

//                 aio = e->data;
//                 aio->res = event[i].res;

//                 ngx_post_event(e, &ngx_posted_events);
//             }

//             continue;
//         }

//         if (events == 0) {
//             return;
//         }

//         /* events == -1 */
//         ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
//                       "io_getevents() failed");
//         return;
//     }
// }

// #endif


// static void *
// ngx_epoll_create_conf(ngx_cycle_t *cycle)
// {
//     ngx_epoll_conf_t  *epcf;

//     epcf = ngx_palloc(cycle->pool, sizeof(ngx_epoll_conf_t));
//     if (epcf == NULL) {
//         return NULL;
//     }

//     epcf->events = NGX_CONF_UNSET;
//     epcf->aio_requests = NGX_CONF_UNSET;

//     return epcf;
// }


// static char *
// ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf)
// {
//     ngx_epoll_conf_t *epcf = conf;

//     ngx_conf_init_uint_value(epcf->events, 512);
//     ngx_conf_init_uint_value(epcf->aio_requests, 32);

//     return NGX_CONF_OK;
// }
