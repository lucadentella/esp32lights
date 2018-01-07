// working modes
#define MODE_MANUAL		1
#define MODE_TIME		2
#define MODE_LIGHT		3

// relay status
#define RELAY_ON		0
#define RELAY_OFF		1

// static headers for HTTP responses
const static char http_html_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/html\n\n";
const static char http_404_hdr[] = "HTTP/1.1 404 NOT FOUND\n\n";