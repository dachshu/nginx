# io_uring Nginx server benchmarks

## Hardware Used
* CPU: ntel(R) Core(TM) i9-9900K CPU @ 3.60GHz 8 (16 HT) cores
* Memory: 32GB

## Software Used
* Version 4.1.0 of [wrk](https://github.com/wg/wrk)
    * Tested with 2 threads
* Version 1.19.5 of [NGINX with io_uring moudle](!@#$%^&) Open Source. Installed it according to [guide](https://docs.nginx.com/nginx/admin-guide/installing-nginx/installing-nginx-open-source/).
    * Tested with 2 worker processes
* Ubuntu 20.04.1 LTS with 5.7.0-050700-generic

## requirements to run the benchmarks
* Before installing NGINX, `make liburing` is required to use `use io_uring;`
* Linux Kernel 5.7 or higher required

## benchmark info
* The asynchronous sendfile using `IORING_OP_SPLICE` is disabled now for the performance issue. The `splice` performance is getting poor after accepting a client. I'm finding the reason of this problem.


## benchmark results
### Requests/sec

* io_uring

| clients    | 0.5KB | 1KB    | 10KB   | 100KB  |
|:----------:|:-----:|:------:|:------:|:------:|
| 100        | 253498| 250531 | 217228 |  76770 |
| 500        | 269599| 265476 | 212668 |  49454 | 

* epoll without sendfile

| clients    | 0.5KB | 1KB    | 10KB   | 100KB  |
|:----------:|:-----:|:------:|:------:|:------:|
| 100        | 156638| 153042 | 145757 |  60186 |
| 500        | 155979| 155624 | 147130 |  60922 | 

* epoll with sendfile

| clients    | 0.5KB | 1KB    | 10KB   | 100KB  |
|:----------:|:-----:|:------:|:------:|:------:|
| 100        | 138641| 136440 | 134712 | 103041 |
| 500        | 134315| 137198 | 138560 | 102729 | 


<img src="benchmarks/RPS_results.png" alt="NGINX io_uring vs epoll RPS benchmarks" width="600"/>

### Avg Latency (ms)

* io_uring

| clients    | 0.5KB | 1KB    | 10KB   | 100KB  |
|:----------:|:-----:|:------:|:------:|:------:|
| 100        | 0.386 |  0.391 |  0.452 |   1.30 |
| 500        | 4.62  |  4.62  |  5.77  |  10.33 | 

* epoll without sendfile

| clients    | 0.5KB | 1KB    | 10KB   | 100KB  |
|:----------:|:-----:|:------:|:------:|:------:|
| 100        | 0.637 | 0.652  | 0.685  |  1.66  |
| 500        | 34.17 | 35.12  | 36.94  |  90.14 | 

* epoll with sendfile

| clients    | 0.5KB | 1KB    | 10KB   | 100KB  |
|:----------:|:-----:|:------:|:------:|:------:|
| 100        | 0.720 | 0.732  | 0.741  |  0.96  |
| 500        | 39.31 | 39.08  | 38.98  |  49.65 | 


<img src="benchmarks/Latency_results.png" alt="NGINX io_uring vs epoll Latency benchmarks" width="600"/>





