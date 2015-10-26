#include "sock.h"

int init_sock(int port)
{
	int serv_sock;
	struct sockaddr_in serv_addr;

	// alloc socket
	if ((serv_sock = socket(PF_INET, SOCK_STREAM, 0)) == -1){
		perror("socket() error");
		exit(1);
	}

	// init structure
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);

	// Bind
	if (bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)	{
		perror("bind() error");
		exit(1);
	}

	// Listen
	if (listen(serv_sock, 5) == -1)	{
		perror("listen() error");
		exit(1);
	}

	return serv_sock;
}
