
default:	build

clean:
	rm -rf Makefile objs

build:
	$(MAKE) -f objs/Makefile

install:
	$(MAKE) -f objs/Makefile install

modules:
	$(MAKE) -f objs/Makefile modules

upgrade:
	/usr/local/nginx/nginx -t

	kill -USR2 `cat /usr/local/nginx/nginx.pid`
	sleep 1
	test -f /usr/local/nginx/nginx.pid.oldbin

	kill -QUIT `cat /usr/local/nginx/nginx.pid.oldbin`
