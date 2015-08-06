/*
 *  SPI-UDP bridge
 */

#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>

#include <stdbool.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <netinet/in.h>
#include <linux/spi/spidev.h>
#include <linux/types.h>

#define PORT (18420)
#define TIMEOUT (1000) //poll timeout in ms
#define SPI_MAX_PACKET_LEN (64) // spidev limit: 64 bytes on sun4i and 128 bytes on sun6i


static int sockfd; //socket file descriptor
static int spifd; //spi file descriptor
static struct sockaddr_in remote_addr, my_addr;

//you should protect this with a mutex
static volatile bool should_quit = false;

static void quit_handler(int signum) {
	printf("Received signal %d\n", signum);
	should_quit = true;
}

static int setup_spi(char *target_spi) {
	__u8 mode = SPI_MODE_0, 
	     lsb = 0, 
	     bits=8;
	__u32 speed = 500000;

	if ((spifd = open(target_spi, O_RDWR)) < 0) {
		perror("Failed to open SPI bus");
		close(spifd);
		return -1;
	}

	if (ioctl(spifd, SPI_IOC_WR_MODE32, &mode) < 0) {
		perror("SPI can't write spi mode");
		close(spifd);
		return -1;
	}
	if (ioctl(spifd, SPI_IOC_WR_LSB_FIRST, &lsb) < 0) {
		perror("SPI cant write_LSB_first");
		close(spifd);
		return -1;
	}
	if (ioctl(spifd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
		perror("SPI cant write bits/word");
		close(spifd);
		return -1;
	}
	if (ioctl(spifd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		perror("SPI speed");
		close(spifd);
		return -1;
	}

	printf("SPI bus on:     %s\n", target_spi);
	printf("mode:           %d\n", mode);
	printf("lsb?:		%d\n", lsb);
	printf("bits per word:  %d\n", bits);
	printf("Hz max:		%d\n", speed);

	return 0;
}

static int setup_udp(char *target_ip, int broadcast) {
	int ret;

	//create socket
	sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sockfd < 0) {
		perror("Failed to create socket");
		return -1;
	}

	//set socket options
	ret = setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
	if (ret != 0) {
		perror("Failed to set socket options");
		goto err_opened;
	}

	//populate my_addr struct
	bzero(&my_addr, sizeof(my_addr)); //set my__addr to '\0'
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	my_addr.sin_port = htons(PORT);

	//bind socket
	ret = bind(sockfd, (struct sockaddr *) &my_addr, sizeof(my_addr));
	if (ret != 0) {
		perror("Failed to bind socket");
		goto err_opened;
	}
	
	//populate remote_addr struct
	bzero(&remote_addr, sizeof(remote_addr));
	remote_addr.sin_family = AF_INET;
	remote_addr.sin_port = htons(PORT);
	ret = inet_pton(AF_INET, target_ip, &remote_addr.sin_addr);
	if (ret != 1) {
		perror("Invalid target IP address");
		goto err_opened;
	}
	

	printf("\nsockfd: %d\n", sockfd);
	printf("my_addr: \n");
	printf("        sin_family: %d\n", my_addr.sin_family);
	printf("	sin_port  : %d\n", ntohs(my_addr.sin_port));
	printf("	sin_addr  : %d\n", ntohl(my_addr.sin_addr.s_addr));
	printf("remote_addr: \n");
	printf("        sin_family: %d\n", remote_addr.sin_family);
	printf("	sin_port  : %d\n", ntohs(remote_addr.sin_port));
	printf("	sin_addr  : %d\n", ntohl(remote_addr.sin_addr.s_addr));

	return 0 ;

err_opened:
	close(sockfd);
	return ret;
}


/* function to read from socket prints message out as (chars) */
static void read_socket(void) {
	size_t recv_len, sent_len;
	uint8_t udp_buffer[SPI_MAX_PACKET_LEN];
	uint8_t write_buffer[SPI_MAX_PACKET_LEN];
	int i = 0;

	recv_len = recv(sockfd, udp_buffer, SPI_MAX_PACKET_LEN, 0);
	if (recv_len > 0) {
		printf("recv_len: %d, received message over UDP: ", (int)recv_len);
		for (i = 0; i < recv_len; i++)
			printf("%c", udp_buffer[i]);
		printf("\n");
		
		//ok, received from UDP, send over SPI now
		sent_len = write(spifd, udp_buffer, recv_len); 
		if (sent_len != recv_len)
			fprintf(stderr, "Error writing to SPI bus.  Expected to send: %u bytes, but sent: %u bytes\n", (unsigned int)recv_len, (unsigned int) sent_len);
	}
}

/* function to read from SPI */
// THIS FUNCITON USES READ(), only supports half duplex, full duplex requires ioctl()
static void spi_read(int fd, int len) {
	unsigned char buffer[32], *bp;
	int ret, sent_len;

	//create limits on read length, 2<len<32
	if (len < 2)
		len = 2;
	else if (len > sizeof(buffer))
		len = sizeof(buffer);
	memset(buffer, 0, sizeof(buffer));

	ret = read(fd, buffer, len);
	if (ret < 0) {
		perror("read");
		return;
	}
	else if (ret != len) {
		fprintf(stderr, "short read\n");
		return;
	}

	printf("length: %2d, status: %2d, returned: %02x %02x,", len, ret, buffer[0], buffer[1]);
	ret -= 2;
	bp = buffer + 2;
	while (ret-- > 0)
		printf(" %02x", *bp++);
	printf("\n");

	//send through UDP now
	sent_len = sendto(sockfd, buffer, len, 0, (struct sockaddr*) &remote_addr, sizeof(remote_addr));
	if (sent_len != len) 
		fprintf(stderr, "Error sending to socket. Expected to send: %u bytes, but sent: %u bytes\n", (unsigned int)len, (unsigned int) sent_len);
}

/*
 * Thread Function: wait for data from SPI bus
 */ 
static void *spi_wait(__attribute__((unused))void *unused) {
	sigset_t sigset;
	int ret;
	char *buffer;
	struct pollfd fd;

	/* ignore signals in this thread, they're handled by the parent */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	if (pthread_sigmask(SIG_BLOCK, &sigset, NULL)) {
		perror("Failed to disable signals in UDP thread");
		pthread_exit(NULL);
	}

	/* wait for incoming data or quit */
	while(!should_quit) {
		spi_read(spifd, 32);
		sleep(1);
	}

	return NULL;
}


/*
 * Thread Function: wait for data from local UDP socket
 */
static void *udp_wait(__attribute__((unused))void *unused) {
	sigset_t sigset;
	int ret;
	struct pollfd fd;

	/* ignore signals in this thread, they're handled by the parent */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	if (pthread_sigmask(SIG_BLOCK, &sigset, NULL)) {
		perror("Failed to disable signals in UDP thread");
		pthread_exit(NULL);
	}

	/* wait for incoming data or quit */
	fd.fd = sockfd;
	fd.events = POLLIN;
	while (!should_quit) {
		ret = poll(&fd, 1, TIMEOUT);
		printf("udp_ret: %d\n", ret);
		if (ret > 0) 
			read_socket();
		else if (ret < 0)
			fprintf(stderr, "Error reading from socket\n");
		fd.revents = 0;
	}

	return NULL;

}



int main(int argc, char *argv[]) {
	int c;
	int broadcast = 0;
	char target_ip[16];
	char spi_path[128];
	pthread_t udp_thread;
	pthread_t spi_thread;
	pthread_attr_t tattr;

	/* catch ctrl-c to quit program safely */
	if (signal(SIGINT, quit_handler) == SIG_ERR) {
		printf(" You pressed ctrl-c, I'm quitting out! ");
		return -1;
	}
	
	/*parse arguments*/
	while ((c = getopt(argc, argv, "abc")) != -1) {
		switch(c) {
			case 'a':
				printf("\npicked a\n");
				break;
			case 'b':
				printf("\nEnable UDP broadcast\n");
				broadcast = 1;
				break;
			case 'c':
				printf("\npicked c\n");
				break;
			default:
				printf("\nbleh");
				break;
		}
	}
	optind--; //optind is the index of the next element of the argv[] vector to be processed
	if ((argc - optind) < 3) {
		printf("spi-bridge [-b] <target_ip> <SPI blockdevice>\n");
		printf("[-b] to enable UDP broadcast\n");
		printf(" e.g. $ mavlink-bridge 127.0.0.1 /dev/spidev1.0\n\n");
		printf(" wrong number of arguments, quitting.\n");
		return -1;
	}
	strcpy(target_ip, argv[1+optind]);
	strcpy(spi_path, argv[2+optind]);
	printf("target_ip: %s\n", target_ip);
	printf("spi_path:  %s\n", spi_path);

	/* setup the UDP connection */
	if (setup_udp(target_ip, broadcast) != 0) {
		perror("Could not open UDP port, quitting out...");
		close(sockfd);
	}

	/* setup the SPI device */
	if (setup_spi(spi_path) != 0) {
		perror("Could not open SPI bus, quitting out...");
		close(spifd);
	}

	/* start UDP and SPI threads, then do nothing until ctrl-c or something dies */
	if (pthread_attr_init(&tattr)) {
		perror("Failed to setup thread attributes");
		pthread_attr_destroy(&tattr); //destroy created attributes
	}
	if (pthread_create(&udp_thread, &tattr, udp_wait, NULL)) {
		perror("Failed to spawn UDP thread, quitting out...");
		pthread_cancel(udp_thread);
		pthread_join(udp_thread, NULL);
	}
	if (pthread_create(&spi_thread, &tattr, spi_wait, NULL)) {
		perror("Failed to spawn SPI thread, quitting out...");
		pthread_cancel(spi_thread);
		pthread_join(spi_thread, NULL);
	}
	pthread_attr_destroy(&tattr);

	/* wait for threads, kill with ctrl-c */
	pthread_join(udp_thread, NULL);
	pthread_join(spi_thread, NULL);

	close(sockfd);
	close(spifd);

	return 0;

}

