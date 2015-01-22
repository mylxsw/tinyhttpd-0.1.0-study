/* J. David's webserver */
/* This is a simple webserver.
 * Created November 1999 by J. David Blackstone.
 * CSE 4344 (Network concepts), Prof. Zeigler
 * University of Texas at Arlington
 */
/* This program compiles for Sparc Solaris 2.6.
 * To compile for Linux:
 *  1) Comment out the #include <pthread.h> line.
 *  2) Comment out the line that defines the variable newthread.
 *  3) Comment out the two lines that run pthread_create().
 *  4) Uncomment the line that runs accept_request().
 *  5) Remove -lsocket from the Makefile.
 */
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <pthread.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"

void accept_request(int);
void bad_request(int);
void cat(int, FILE *);
void cannot_execute(int);
void error_die(const char *);
void execute_cgi(int, const char *, const char *, const char *);
int get_line(int, char *, int);
void headers(int, const char *);
void not_found(int);
void serve_file(int, const char *);
int startup(u_short *);
void unimplemented(int);

/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the client */
/**********************************************************************/
void accept_request(int client) {
	char buf[1024];
	int numchars;
	char method[255];
	char url[255];
	char path[512];
	size_t i, j;
	struct stat st;
	int cgi = 0; /* becomes true if server decides this is a CGI
	 * program */
	char *query_string = NULL;

	// 读取Http请求的第一行，获取请求方法
	numchars = get_line(client, buf, sizeof(buf));
	i = 0;
	j = 0;
	// 请求方法为第一行第一个空格前的内容
	while (!ISspace(buf[j]) && (i < sizeof(method) - 1)) {
		method[i] = buf[j];
		i++;
		j++;
	}
	method[i] = '\0';

	// 只支持GET/POST方法
	if (strcasecmp(method, "GET") && strcasecmp(method, "POST")) {
		unimplemented(client);
		return;
	}

	// 如果是POST方法，则启用CGI模式
	if (strcasecmp(method, "POST") == 0)
		cgi = 1;

	i = 0;
	// 跳过METHOD和URL中间的空格
	while (ISspace(buf[j]) && (j < sizeof(buf)))
		j++;
	// 提取请求URL
	while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf))) {
		url[i] = buf[j];
		i++;
		j++;
	}
	url[i] = '\0';

	// 如果是GET方法，读取query string
	if (strcasecmp(method, "GET") == 0) {
		query_string = url;
		// 从url开始部分，将query_string指针移动到?开始处
		while ((*query_string != '?') && (*query_string != '\0'))
			query_string++;

		// 如果找到了?，则启用CGI，获取?后面部分作为query string
		if (*query_string == '?') {
			cgi = 1;
			// 这样就将query_string和url访问的资源分离（url\0query_string\0）
			*query_string = '\0';
			query_string++;
		}
	}

	// 设置目录为htdocs+请求url
	sprintf(path, "htdocs%s", url);
	// 如果最后访问的是目录，则使用默认文件index.html
	if (path[strlen(path) - 1] == '/')
		strcat(path, "index.html");

	// 获取访问文件的信息
	if (stat(path, &st) == -1) {
		// 获取文件信息失败，读取整个header并抛弃
		while ((numchars > 0) && strcmp("\n", buf)) /* read & discard headers */
			numchars = get_line(client, buf, sizeof(buf));

		// 抛出找不到页面404
		not_found(client);
	} else {
		// st.st_mode & S_IFMT获取文件类型，这里判断文件类型是否是目录
		// 如果是目录，则访问目录下的index.html文件
		if ((st.st_mode & S_IFMT) == S_IFDIR)
			strcat(path, "/index.html");
		// 判断文件的用户或者组用户、其它用户的执行、搜索权限
		// 如果可执行，则设置为CGI模式
		if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP)
				|| (st.st_mode & S_IXOTH))
			cgi = 1;
		// 如果不能执行为CGI，则直接返回文件信息
		// 否则执行CGI程序
		if (!cgi)
			serve_file(client, path);
		else
			execute_cgi(client, path, method, query_string);
	}

	close(client);
}

/**********************************************************************/
/* Inform the client that a request it has made has a problem.
 * Parameters: client socket */
/**********************************************************************/
void bad_request(int client) {
	char buf[1024];

	sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "<P>Your browser sent a bad request, ");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "such as a POST without a Content-Length.\r\n");
	send(client, buf, sizeof(buf), 0);
}

/**********************************************************************/
/* Put the entire contents of a file out on a socket.  This function
 * is named after the UNIX "cat" command, because it might have been
 * easier just to do something like pipe, fork, and exec("cat").
 * Parameters: the client socket descriptor
 *             FILE pointer for the file to cat */
/**********************************************************************/
void cat(int client, FILE *resource) {
	char buf[1024];

	fgets(buf, sizeof(buf), resource);
	while (!feof(resource)) {
		send(client, buf, strlen(buf), 0);
		fgets(buf, sizeof(buf), resource);
	}
}

/**********************************************************************/
/* Inform the client that a CGI script could not be executed.
 * Parameter: the client socket descriptor. */
/**********************************************************************/
void cannot_execute(int client) {
	char buf[1024];

	sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Print out an error message with perror() (for system errors; based
 * on value of errno, which indicates system call errors) and exit the
 * program indicating an error. */
/**********************************************************************/
void error_die(const char *sc) {
	perror(sc);
	exit(1);
}

/**********************************************************************/
/* Execute a CGI script.  Will need to set environment variables as
 * appropriate.
 * Parameters: client socket descriptor
 *             path to the CGI script */
/**********************************************************************/
void execute_cgi(int client, const char *path, const char *method,
		const char *query_string) {
	char buf[1024];
	int cgi_output[2];
	int cgi_input[2];
	pid_t pid;
	int status;
	int i;
	char c;
	int numchars = 1;
	int content_length = -1;

	buf[0] = 'A';
	buf[1] = '\0';
	if (strcasecmp(method, "GET") == 0) {
		// 如果是GET方法，则去掉header
		while ((numchars > 0) && strcmp("\n", buf)) /* read & discard headers */
			numchars = get_line(client, buf, sizeof(buf));
	} else /* POST */ {
		numchars = get_line(client, buf, sizeof(buf));
		while ((numchars > 0) && strcmp("\n", buf)) {
			// 用一个15字符的缓冲区读取header，判断是否是Content-Length
			// 读取到header中的Content-Length字段值
			buf[15] = '\0';
			if (strcasecmp(buf, "Content-Length:") == 0)
				content_length = atoi(&(buf[16]));
			numchars = get_line(client, buf, sizeof(buf));
		}
		// 如果没有设置Content-Length或者设置为-1，则报错
		if (content_length == -1) {
			bad_request(client);
			return;
		}
	}

	// 发送Response Header
	sprintf(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);

	// 创建来自CGI输出的管道
	if (pipe(cgi_output) < 0) {
		cannot_execute(client);
		return;
	}

	// 创建输入到CGI的管道
	if (pipe(cgi_input) < 0) {
		cannot_execute(client);
		return;
	}

	// 创建子进程，在子进程中执行CGI脚本
	if ((pid = fork()) < 0) {
		cannot_execute(client);
		return;
	}
	if (pid == 0) /* child: CGI script */
	{
		char meth_env[255];
		char query_env[255];
		char length_env[255];

		// 将CGI的标准输出设置为管道输出
		dup2(cgi_output[1], 1);
		// 将CGI的标准输入设置为来自管道的标准输入
		dup2(cgi_input[0], 0);
		close(cgi_output[0]);
		close(cgi_input[1]);

		// 设置环境变量REQUEST_METHOD
		sprintf(meth_env, "REQUEST_METHOD=%s", method);
		putenv(meth_env);
		// 如果请求为GET请求，则设置环境变量QUERY_STRING
		if (strcasecmp(method, "GET") == 0) {
			sprintf(query_env, "QUERY_STRING=%s", query_string);
			putenv(query_env);
		} else { /* POST */
			// 如果是POST请求，则设置CONTENT_LENGTH环境变量
			sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
			putenv(length_env);
		}
		// 执行CGI脚本，调用excel之后，子进程被取代
		execl(path, path, NULL);
		exit(0);
	} else { /* parent */
		close(cgi_output[1]);
		close(cgi_input[0]);
		// 如果是POST方法，则将POST数据写给CGI脚本
		if (strcasecmp(method, "POST") == 0)
			for (i = 0; i < content_length; i++) {
				recv(client, &c, 1, 0);
				write(cgi_input[1], &c, 1);
			}
		// 从CGI脚本读取输出并发送到浏览器
		while (read(cgi_output[0], &c, 1) > 0)
			send(client, &c, 1, 0);

		close(cgi_output[0]);
		close(cgi_input[1]);

		// 等待子进程结束
		waitpid(pid, &status, 0);
	}
}

/**********************************************************************/
/* Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) */
/**********************************************************************/
int get_line(int sock, char *buf, int size) {
	int i = 0;
	char c = '\0';
	int n;

	while ((i < size - 1) && (c != '\n')) {
		n = recv(sock, &c, 1, 0);
		/* DEBUG printf("%02X\n", c); */
		if (n > 0) {
			if (c == '\r') {
				n = recv(sock, &c, 1, MSG_PEEK);
				/* DEBUG printf("%02X\n", c); */
				if ((n > 0) && (c == '\n'))
					recv(sock, &c, 1, 0);
				else
					c = '\n';
			}
			buf[i] = c;
			i++;
		} else
			c = '\n';
	}
	buf[i] = '\0';

	return (i);
}

/**********************************************************************/
/* Return the informational HTTP headers about a file. */
/* Parameters: the socket to print the headers on
 *             the name of the file */
/**********************************************************************/
void headers(int client, const char *filename) {
	char buf[1024];
	(void) filename; /* could use filename to determine file type */

	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Give a client a 404 not found status message. */
/**********************************************************************/
void not_found(int client) {
	char buf[1024];

	sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "your request because the resource specified\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "is unavailable or nonexistent.\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Send a regular file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve */
/**********************************************************************/
void serve_file(int client, const char *filename) {
	FILE *resource = NULL;
	int numchars = 1;
	char buf[1024];

	// 设置buf[0]为任意字符，这里是防止下面的while第一次就是false而无法去掉header
	// numchars初始设为1也是同样的目的
	buf[0] = 'A';
	buf[1] = '\0';
	// 逐行读取header并抛弃
	while ((numchars > 0) && strcmp("\n", buf)) /* read & discard headers */
		numchars = get_line(client, buf, sizeof(buf));

	// 只读方式打开文件
	resource = fopen(filename, "r");
	if (resource == NULL)
		not_found(client);
	else {
		// 发送文件内容，首先发送header
		headers(client, filename);
		cat(client, resource);
	}
	fclose(resource);
}

/**********************************************************************/
/* This function starts the process of listening for web connections
 * on a specified port.  If the port is 0, then dynamically allocate a
 * port and modify the original port variable to reflect the actual
 * port.
 * Parameters: pointer to variable containing the port to connect on
 * Returns: the socket */
/**********************************************************************/
int startup(u_short *port) {
	int httpd = 0;
	struct sockaddr_in name;

	// PF_INET指定为IPV4协议簇，SOCK_STREAM流式套接字（TCP），协议
	httpd = socket(PF_INET, SOCK_STREAM, 0);
	if (httpd == -1)
		error_die("socket");
	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = htons(*port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);

	// 绑定socket
	if (bind(httpd, (struct sockaddr *) &name, sizeof(name)) < 0)
		error_die("bind");
	if (*port == 0) /* 如果port为0，则为自动分配端口号，下面需要从socket获取这个端口号以便显示 */
	{
		int namelen = sizeof(name);
		if (getsockname(httpd, (struct sockaddr *) &name, &namelen) == -1)
			error_die("getsockname");
		*port = ntohs(name.sin_port);
	}

	// 最大支持5个客户端等待连接
	if (listen(httpd, 5) < 0)
		error_die("listen");
	return (httpd);
}

/**********************************************************************/
/* Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket */
/**********************************************************************/
void unimplemented(int client) {
	char buf[1024];

	sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</TITLE></HEAD>\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/

int main(void) {
	int server_sock = -1;
	u_short port = 0;
	int client_sock = -1;
	struct sockaddr_in client_name;
	int client_name_len = sizeof(client_name);
	pthread_t newthread;

	server_sock = startup(&port);
	printf("httpd running on port %d\n", port);

	while (1) {
		client_sock = accept(server_sock, (struct sockaddr *) &client_name,
				&client_name_len);
		if (client_sock == -1)
			error_die("accept");
//		accept_request(client_sock);

		// 线程， 线程参数， 回调函数， 回调函数参数
		if (pthread_create(&newthread, NULL, accept_request, client_sock) != 0)
			perror("pthread_create");
	}

	close(server_sock);

	return (0);
}
