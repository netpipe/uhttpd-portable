# uhttpd-portable
portable uhttpd client for other linux distributions with CGI/PHP support. ported from OpenWRT


compile with codeblocks for now so you dont need to compile libubox

./Client -p 127.0.0.1:8084 -f -h ./ -c ./files/uhttpd.config -i .php=/usr/bin/php-cgi

"which php-cgi" to get your path

  to start it
