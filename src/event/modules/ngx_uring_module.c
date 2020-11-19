
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include "liburing.h"


typedef struct {
    ngx_uint_t entries;
} ngx_uring_conf_t;


typedef struct {
    ngx_connection_t   *conn;
    ngx_uint_t            ev;
} ngx_uring_info_t;


static ngx_int_t ngx_uring_init(ngx_cycle_t *cycle, ngx_msec_t timer);
static void ngx_uring_done(ngx_cycle_t *cycle);
static ngx_int_t ngx_uring_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_uring_add_connection(ngx_connection_t *c);
static ngx_int_t ngx_uring_del_connection(ngx_connection_t *c,
    ngx_uint_t flags);
static ngx_int_t ngx_uring_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
    ngx_uint_t flags);

static void *ngx_uring_create_conf(ngx_cycle_t *cycle);
static char *ngx_uring_init_conf(ngx_cycle_t *cycle, void *conf);


static ngx_int_t ngx_uring_accept(ngx_connection_t *c);
static ssize_t ngx_uring_recv(ngx_connection_t *c, u_char *buf, size_t size);
#if (NGX_USE_URING_SPLICE)
static ngx_chain_t *ngx_uring_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit);
static ssize_t ngx_uring_splice_sendfile(ngx_connection_t *c, ngx_buf_t *file, size_t size);
#endif
ssize_t ngx_uring_writev(ngx_connection_t *c, int nelts, int start_el);
ngx_chain_t * ngx_uring_writev_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit);
ssize_t ngx_uring_readv_chain(ngx_connection_t *c, ngx_chain_t *chain, off_t limit);
ssize_t ngx_uring_send(ngx_connection_t *c, u_char *buf, size_t size);


static struct io_uring ring;

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
        NULL,                            /* trigger a notify */
        ngx_uring_process_events,        /* process the events */
        ngx_uring_init,                  /* init the events */
        ngx_uring_done,                  /* done the events */
    }
};

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

ngx_os_io_t ngx_uring_io = {
    ngx_uring_recv,
    ngx_uring_readv_chain, 
    NULL,                   /* udp recv */
    ngx_uring_send,
    NULL,                   /* udp send */
    NULL,                   /* udp sendmsg chain */
#if (NGX_USE_URING_SPLICE)
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
    ngx_uring_conf_t        *urcf;
    struct io_uring_params   params;

    urcf = ngx_event_get_conf(cycle->conf_ctx, ngx_uring_module);

    if(ring.ring_fd == 0){
        ngx_memzero(&params, sizeof(params));
        if (io_uring_queue_init_params(urcf->entries, &ring, &params) < 0) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "io_uring_queue_init_params() failed");
            return NGX_ERROR;
        }
        
        if(!(params.features & IORING_FEAT_FAST_POLL)){
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "IORING_FEAT_FAST_POLL is not available");
            return NGX_ERROR;
        }
    }

    ngx_io = ngx_uring_io;

    ngx_event_actions = ngx_uring_module_ctx.actions;

#if (NGX_HAVE_CLEAR_EVENT)
    ngx_event_flags = NGX_USE_CLEAR_EVENT
#else
    ngx_event_flags = NGX_USE_LEVEL_EVENT
#endif
                      |NGX_USE_GREEDY_EVENT
                      |NGX_USE_URING_EVENT;

    return NGX_OK;
}



static void
ngx_uring_done(ngx_cycle_t *cycle)
{
    io_uring_queue_exit(&ring);
    ngx_memset(&ring, 0, sizeof(ring));
}


static ngx_int_t
ngx_uring_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_connection_t    *c;        

    c = ev->data;

    if(event == NGX_READ_EVENT && c->read->accept){
        if(ngx_uring_accept(c) == NGX_ERROR)
            return NGX_ERROR;
    }

    ev->active = 1;
    ev->ready = 1;

    return NGX_OK;
}


static ngx_int_t
ngx_uring_add_connection(ngx_connection_t *c)
{
    if(c->read->accept){
        if(ngx_uring_accept(c) == NGX_ERROR)
            return NGX_ERROR;
    }

    c->read->active = 1;
    c->write->active = 1;
    c->read->ready = 1;
    c->write->ready = 1;

    return NGX_OK;
}


static ngx_int_t
ngx_uring_del_connection(ngx_connection_t *c, ngx_uint_t flags)
{
    c->read->active = 0;
    c->write->active = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_uring_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    unsigned             head, count;
    ngx_event_t         *rev, *wev;
    ngx_connection_t    *c;
    struct io_uring_cqe *cqe;
    ngx_uring_info_t    *ui;

    /* NGX_TIMER_INFINITE == INFTIM */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "io_uring timer: %M", timer);

    io_uring_submit_and_wait(&ring, 1);
    
    if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm) {
        ngx_time_update();
    }

    count = 0;
    
    io_uring_for_each_cqe(&ring, head, cqe) {
        ++count;
        ui = (ngx_uring_info_t*)cqe->user_data;
        c = ui->conn;

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
                       c->fd, ui->ev, cqe->user_data);

        rev = c->read;
        wev = c->write;

        switch (ui->ev)
        {
        case NGX_URING_ACCEPT:{
            ngx_pfree(c->pool, ui);
            if(cqe->res != -11) {
                rev->uring_res = cqe->res;

                rev->ready = 1;
                rev->complete = 1;
                rev->available = -1;

                if (flags & NGX_POST_EVENTS) {
                    ngx_post_event(rev, &ngx_posted_accept_events);

                } else {
                    rev->handler(rev);
                }
            }
            
            rev->ready = 0;
            rev->complete = 0;
            rev->available = 1;

            if(ngx_uring_accept(c) == NGX_ERROR)
                return NGX_ERROR;
            break;
        }
        case NGX_URING_READ:
        case NGX_URING_READV:{
            ngx_pfree(c->pool, ui);
            rev->uring_res = cqe->res;
            rev->ready = 1;
            rev->complete = 1;
            rev->available = -1;

            if (flags & NGX_POST_EVENTS) {
                ngx_post_event(rev, &ngx_posted_events);

            } else {
                rev->handler(rev);
            }
            break;
        }
        case NGX_URING_SEND:
        case NGX_URING_WRITEV:{
            ngx_pfree(c->pool, ui);
            wev->uring_pending -= 1;
            wev->uring_res += cqe->res;
            
            if(wev->uring_pending == 0) {
                wev->complete = 1;
                wev->ready = 1;
                wev->available = -1;

                if (flags & NGX_POST_EVENTS) {
                    ngx_post_event(wev, &ngx_posted_events);

                } else {
                    wev->handler(wev);
                }
            }
            break;
        }
        case NGX_URING_SPLICE_TO_PIPE:{
            ngx_pfree(c->pool, ui);
            break;
        }
        case NGX_URING_SPLICE_FROM_PIPE:{
            ngx_pfree(c->pool, ui);
            wev->uring_pending -= 1;
            wev->uring_res += cqe->res;

            if(wev->uring_pending == 0) {
                wev->complete = 1;
                wev->ready = 1;
                wev->available = -1;

                if (flags & NGX_POST_EVENTS) {
                    ngx_post_event(wev, &ngx_posted_events);

                } else {
                    wev->handler(wev);
                }
            }
            break;
        }
        default:
            ngx_pfree(c->pool, ui);
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "io_uring invalid event type");
            return NGX_ERROR;
        }

    }
        
    io_uring_cq_advance(&ring, count);

    if (count == 0) {
        if (timer != NGX_TIMER_INFINITE) {
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "io_uring_submit_and_wait() returned no events without timeout");
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void *
ngx_uring_create_conf(ngx_cycle_t *cycle)
{
    ngx_uring_conf_t  *urcf;

    urcf = ngx_palloc(cycle->pool, sizeof(ngx_uring_conf_t));
    if (urcf == NULL) {
        return NULL;
    }

    urcf->entries = NGX_CONF_UNSET;

    return urcf;
}


static char *
ngx_uring_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_uring_conf_t *urcf = conf;

    ngx_conf_init_uint_value(urcf->entries, 32768);

    return NGX_CONF_OK;
}


static ngx_int_t 
ngx_uring_accept(ngx_connection_t *c)
{
    struct io_uring_sqe *sqe;
    ngx_uring_info_t    *ui;

    if(c->pool == NULL){
        c->pool = ngx_create_pool(c->listening->pool_size, c->log);
        if (c->pool == NULL) {
            return NGX_ERROR;
        }
    }

    if(c->sockaddr == NULL){
        c->sockaddr = ngx_palloc(c->pool, sizeof(ngx_sockaddr_t));
        if (c->sockaddr == NULL) {
            return NGX_ERROR;
        }
        c->socklen = sizeof(ngx_sockaddr_t);
    }
    
    ui = ngx_palloc(c->pool, sizeof(ngx_uring_info_t));
    ui->conn = c;
    ui->ev = NGX_URING_ACCEPT;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
            "io_uring prep event: fd:%d op:%d ",
            c->fd, ui->ev);

    sqe = io_uring_get_sqe(&ring);
    if(sqe == NULL){
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                  "io_uring_get_sqe() failed");
        return NGX_ERROR;
    }

    io_uring_prep_accept(sqe, c->fd, c->sockaddr, &c->socklen, 0);
    io_uring_sqe_set_data(sqe, ui);

    return NGX_OK;
}


ssize_t
ngx_uring_recv(ngx_connection_t *c, u_char *buf, size_t size)
{
    ssize_t              n;
    ngx_event_t         *rev;
    ngx_uring_info_t    *ui;
    struct io_uring_sqe *sqe;
    
    rev = c->read;

    if(!rev->complete && rev->uring_pending) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0, "second uring_recv post");
        return NGX_AGAIN;
    }

    if(rev->complete) {
        n = rev->uring_res;
        rev->uring_res = 0;
        rev->available = 0;
        rev->uring_pending = 0;
        rev->complete = 0;
        
        if(n == 0){
            rev->ready = 0;
            rev->eof = 1;
            return 0;
        }
        if(n < 0){
            ngx_connection_error(c, 0, "uring_recv() failed");
            rev->error = 1;
            return NGX_ERROR;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "uring_recv: fd:%d %ul of %z",
                           c->fd, n, size);

        return n;
    }

    ui = ngx_palloc(c->pool, sizeof(ngx_uring_info_t));
    ui->conn = c;
    ui->ev = NGX_URING_READ;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
            "io_uring prep event: fd:%d op:%d ",
            c->fd, ui->ev);
    
    sqe = io_uring_get_sqe(&ring);
    if(sqe == NULL){
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                  "io_uring_get_sqe() failed");
        return NGX_ERROR;
    }

    io_uring_prep_recv(sqe, c->fd, buf, size, 0);
    io_uring_sqe_set_data(sqe, ui);

    rev->complete = 0;
    rev->ready = 0;
    rev->uring_pending = 1;
    
    return NGX_AGAIN;
}


#define NGX_SENDFILE_MAXSIZE  2147483647L

ssize_t
ngx_uring_writev(ngx_connection_t *c, int nelts, int start_el)
{
    ngx_uring_info_t    *ui;
    struct io_uring_sqe *sqe;

    ui = ngx_palloc(c->pool, sizeof(ngx_uring_info_t));
    ui->conn = c;
    ui->ev = NGX_URING_WRITEV;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
            "io_uring prep event: fd:%d op:%d ",
            c->fd, ui->ev);
    
    sqe = io_uring_get_sqe(&ring);
    if(sqe == NULL){
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                  "io_uring_get_sqe() failed");
        return NGX_ERROR;
    }

    io_uring_prep_writev(sqe, c->fd, &c->write->uring_iov[start_el], nelts - start_el , 0);
    io_uring_sqe_set_data(sqe, ui);

    return NGX_AGAIN;
}


ngx_chain_t *
ngx_uring_writev_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    u_char        *prev;
    ssize_t        n, sent, size;
    off_t          send;
    ngx_chain_t   *cl;
    ngx_event_t   *wev;
    int            nelts, start_el;

    wev = c->write;

    if(wev->uring_pending && !wev->complete){
        return in;
    }

    if(wev->complete){
        sent = wev->uring_res;

        wev->complete = 0;
        wev->uring_pending = 0;
        wev->uring_res = 0;
        wev->ready = 1;

        if(sent != wev->uring_rq_size){
            ngx_connection_error(c, 0, "uring_writev_chain failed");
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
            wev->uring_iov[nelts - 1].iov_len += cl->buf->last - cl->buf->pos;

        } else {
            ++nelts;
            if (nelts >= NGX_IOVS_PREALLOCATE) {
                wev->error = 1;
                return NGX_CHAIN_ERROR;
            }
            wev->uring_iov[nelts - 1].iov_base = (void *) cl->buf->pos;
            wev->uring_iov[nelts - 1].iov_len = cl->buf->last - cl->buf->pos;
        }
        prev = cl->buf->last;
        send += size;
        cl = cl->next;
    }

    if(nelts - start_el > 0){
        n = ngx_uring_writev(c, nelts, start_el);

        if (n == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        sent = 0;
    }
    
    wev->uring_rq_size = send;
    wev->uring_pending = 1;
    wev->complete = 0;
    wev->ready = 0;

    return in;
}

#if (NGX_USE_URING_SPLICE)
ngx_chain_t *
ngx_uring_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    u_char        *prev;
    off_t          send;
    size_t         file_size;
    ngx_buf_t     *file;
    ngx_event_t   *wev;
    ngx_chain_t   *cl;
    ssize_t        n, sent, size;
    int            nelts, start_el, pending;

    wev = c->write;

    if(wev->uring_pending && !wev->complete){
        return in;
    }
    
    if(wev->complete){
        sent = wev->uring_res;

        wev->complete = 0;
        wev->uring_pending = 0;
        wev->uring_res = 0;
        wev->ready = 1;

        if(sent != wev->uring_rq_size){
            ngx_connection_error(c, 0, "uring_writev_chain failed");
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
    pending = 0;

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
                ++pending;
                n = ngx_uring_writev(c, nelts, start_el);

                if (n == NGX_ERROR) {
                    return NGX_CHAIN_ERROR;
                }

                start_el += nelts;
                prev = NULL;
            }
            
            file = cl->buf;

            /* coalesce the neighbouring file bufs */

            file_size = (size_t) ngx_chain_coalesce_file(&cl, limit - send);

            send += file_size;

            if (file_size == 0) {
                ngx_debug_point();
                return NGX_CHAIN_ERROR;
            }
            
            n = ngx_uring_splice_sendfile(c, file, file_size);   

            if (n == NGX_ERROR) {
                wev->error = 1;
                return NGX_CHAIN_ERROR;
            }
            
            ++pending;
            continue;
        }

        size = cl->buf->last - cl->buf->pos;
        if (send + size > limit) {
            size = (u_long) (limit - send);
        }

        if (prev == cl->buf->pos) {
            wev->uring_iov[nelts - 1].iov_len += cl->buf->last - cl->buf->pos;

        } else {
            ++nelts;
            if (nelts >= NGX_IOVS_PREALLOCATE) {
                wev->error = 1;
                return NGX_CHAIN_ERROR;
            }
            wev->uring_iov[nelts - 1].iov_base = (void *) cl->buf->pos;
            wev->uring_iov[nelts - 1].iov_len = cl->buf->last - cl->buf->pos;
        }

        prev = cl->buf->last;
        send += size;
        cl = cl->next;
    }

    if(nelts - start_el > 0){
        ++pending;
        n = ngx_uring_writev(c, nelts, start_el);

        if (n == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }
    }
    
    wev->uring_rq_size = send;
    wev->uring_pending = pending;
    wev->complete = 0;
    wev->ready = 0;

    return in;
}



static ssize_t
ngx_uring_splice_sendfile(ngx_connection_t *c, ngx_buf_t *file, size_t size)
{
    ngx_uring_info_t    *ui;
    struct io_uring_sqe *sqe;
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
    
    ui = ngx_palloc(c->pool, sizeof(ngx_uring_info_t));
    ui->conn = c;
    ui->ev = NGX_URING_SPLICE_TO_PIPE;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
            "io_uring prep event: fd:%d op:%d ",
            c->fd, ui->ev);
    
    sqe = io_uring_get_sqe(&ring);
    if(sqe == NULL){
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                  "io_uring_get_sqe() failed");
        return NGX_ERROR;
    }
    io_uring_prep_splice(sqe, file->file->fd, offset, 
                         c->write->uring_splice_pipe[1], -1, size,  SPLICE_F_MOVE | SPLICE_F_MORE);
    sqe->flags = IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe, ui);


    ui = ngx_palloc(c->pool, sizeof(ngx_uring_info_t));
    ui->conn = c;
    ui->ev = NGX_URING_SPLICE_FROM_PIPE;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
            "io_uring prep event: fd:%d op:%d ",
            c->fd, ui->ev);
    
    sqe = io_uring_get_sqe(&ring);
    if(sqe == NULL){
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                  "io_uring_get_sqe() failed");
        return NGX_ERROR;
    }
    io_uring_prep_splice(sqe, c->write->uring_splice_pipe[0], -1, 
                         c->fd, -1, size, SPLICE_F_MOVE | SPLICE_F_MORE);
    io_uring_sqe_set_data(sqe, ui);


    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "uring_splice_sendfile: @%O %uz", file->file_pos, size);

    return NGX_AGAIN;
}
#endif

ssize_t
ngx_uring_readv_chain(ngx_connection_t *c, ngx_chain_t *chain, off_t limit)
{
    u_char              *prev;
    ssize_t              n, size;
    ngx_array_t          vec;
    ngx_event_t         *rev;
    struct iovec        *iov;
    ngx_uring_info_t    *ui;
    struct io_uring_sqe *sqe;

    rev = c->read;

    if(!rev->complete && rev->uring_pending) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0, "second uring_readv_chain post");
        return NGX_AGAIN;
    }

    if(rev->complete) {
        n = rev->uring_res;
        rev->uring_res = 0;
        rev->available = 0;
        rev->uring_pending = 0;
        rev->complete = 0;
        
        if(n == 0){
            rev->ready = 0;
            rev->eof = 1;
            return 0;
        }
        if(n < 0){
            ngx_connection_error(c, 0, "uring_readv() failed");
            rev->error = 1;
            return NGX_ERROR;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "uring_readv: fd:%d %ul of %z",
                           c->fd, n, size);

        return n;
    }

    prev = NULL;
    iov = NULL;
    size = 0;

    vec.elts = rev->uring_iov;
    vec.nelts = 0;
    vec.size = sizeof(struct iovec);
    vec.nalloc = NGX_IOVS_PREALLOCATE;
    vec.pool = c->pool;

    /* coalesce the neighbouring bufs */

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
            iov->iov_len += n;

        } else {
            if (vec.nelts >= IOV_MAX) {
                break;
            }

            iov = ngx_array_push(&vec);
            if (iov == NULL) {
                return NGX_ERROR;
            }

            iov->iov_base = (void *) chain->buf->last;
            iov->iov_len = n;
        }

        size += n;
        prev = chain->buf->end;
        chain = chain->next;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "readv: %ui, last:%uz", vec.nelts, iov->iov_len);

    ui = ngx_palloc(c->pool, sizeof(ngx_uring_info_t));
    ui->conn = c;
    ui->ev = NGX_URING_READV;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
            "io_uring prep event: fd:%d op:%d ",
            c->fd, ui->ev);
    
    sqe = io_uring_get_sqe(&ring);
    if(sqe == NULL){
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                  "io_uring_get_sqe() failed");
        return NGX_ERROR;
    }

    io_uring_prep_readv(sqe, c->fd, (struct iovec *) vec.elts, vec.nelts, 0);
    io_uring_sqe_set_data(sqe, ui);

    rev->complete = 0;
    rev->ready = 0;
    rev->uring_pending = 1;

    return NGX_AGAIN;
}


ssize_t
ngx_uring_send(ngx_connection_t *c, u_char *buf, size_t size)
{
    ssize_t              n;
    ngx_event_t         *wev;
    ngx_uring_info_t    *ui;
    struct io_uring_sqe *sqe;

    wev = c->write;

    if(wev->uring_pending && !wev->complete){
        return NGX_AGAIN;
    }

    if(wev->complete){
        n = wev->uring_res;
        wev->complete = 0;
        wev->uring_pending = 0;
        wev->uring_res = 0;
        wev->ready = 1;

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "send: fd:%d %z of %uz", c->fd, n, size);
        
        if (n == 0) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0, "send() returned zero");
            wev->ready = 0;
            return n;
        }

        if (n > 0) {
            if (n < (ssize_t) size) {
                wev->ready = 0;
            }

            c->sent += n;

            return n;
        }


        wev->error = 1;
        (void) ngx_connection_error(c, 0, "send() failed");
        return NGX_ERROR;

    }

    ui = ngx_palloc(c->pool, sizeof(ngx_uring_info_t));
    ui->conn = c;
    ui->ev = NGX_URING_SEND;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
            "io_uring prep event: fd:%d op:%d ",
            c->fd, ui->ev);
    
    sqe = io_uring_get_sqe(&ring);
    if(sqe == NULL){
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                  "io_uring_get_sqe() failed");
        return NGX_ERROR;
    }

    io_uring_prep_send(sqe, c->fd, buf, size, 0);
    io_uring_sqe_set_data(sqe, ui);

    wev->uring_rq_size = size;
    wev->uring_pending = 1;
    wev->complete = 0;
    wev->ready = 0;

    return NGX_AGAIN;
}

