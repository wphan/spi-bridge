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

#include <linux/spi/spidev.h>
#include <linux/types.h>
#include <sys/ioctl.h>

#include <stdbool.h>

#include <netinet/in.h>

#define PORT (18420)
#define TIMEOUT (1000) //poll timeout in ms
#define SPI_MAX_PACKET_LEN (64) // spidev limit: 64 bytes on sun4i and 128 bytes on sun6i


static int sockfd; //socket file descriptor
static int spifd; //spi file descriptor
static struct sockaddr_in remote_addr, my_addr;
//SPI variables
static unsigned char spi_mode = SPI_MODE_0;
static unsigned char spi_bitsPerWord = 8;
static unsigned int spi_speed = 1000000;
static uint8_t *spi_tx;
static uint8_t *spi_rx;

//you should protect this with a mutex
static volatile bool should_quit = false;

static void quit_handler(int signum) {
	printf("Received signal %d\n", signum);
	should_quit = true;
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
	size_t recv_len, to_write, wrote_len;
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

	}
}

/* function to read from SPI bus, prrints message out as (chars) */
static void read_spi(void) {

}

/*
 * Thread Function: wait for data from SPI bus and forward through UDP
 */ 
static void *spi_wait(__attribute__((unused))void *unused) {
	sigset_t sigset;
	int ret;
	struct pollfd fd;
	struct spi_ioc_transfer tr = {
		.tx_buf = (unsigned long)spi_tx,
		.rx_buf = (unsigned long)spi_rx,
		.len = SPI_MAX_PACKET_LEN,
		.delay_usecs = 1,
		.speed_hz = spi_speed,
		.bits_per_word = spi_bitsPerWord,
	};

	spi_tx = malloc(SPI_MAX_PACKET_LEN);
	spi_rx = malloc(SPI_MAX_PACKET_LEN);

	/* ignore signals in this thread, they're handled by the parent */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	if (pthread_sigmask(SIG_BLOCK, &sigset, NULL)) {
		perror("Failed to disable signals in UDP thread");
		pthread_exit(NULL);
	}

	//see if theres stufff
	ret = ioctl(spifd, SPI_IOC_MESSAGE(1), &tr)
	if (ret<1)
		perror("bleh broken");
	/* wait for incoming data or quit */
/*	
	fd.fd = spifd;
	fd.events = POLLIN;
	while (!should_quit) {
		ret = poll(&fd, 1, TIMEOUT);
		if (ret > 0)
			read_spi();
		else
			fprintf(stderr, "Error reading from SPI device\n");
		fd.revents = 0;
	}
*/
}


/*
 * Thread Function: wait for data from local UDP socket and forward through SPI
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
		printf("ret: %d\n", ret);
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
	//spi specific stuff
	spi_mode = SPI_MODE_0;

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
		printf("spi-bridge [-a] [-b] [-c] <target_ip> <SPI blockdevice>\n");
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

	/* setup the SPI device, stuck all this here b/c making a function did bad things, 
	 * SPI_MODE: 0
	 * BITS_PER_WORD: 8
	 * SPEED: 1000000
	 */ 
	spifd = open(spi_path, O_RDWR);
	if (spifd < 0) {
		perror("could not open spi device");	
		close(spifd);
		return 0;
	}
	if (ioctl(spifd, SPI_IOC_WR_MODE, &spi_mode) < 0) {
		perror("could not set SPIMode(WR), ioctl failed");
		close(spifd);
		return 0;
	}
	if (ioctl(spifd, SPI_IOC_RD_MODE, &spi_mode) < 0) {
		perror("could not set SPIMODE(RD), ioctl failed");
		close(spifd);
		return 0;
	}
	if (ioctl(spifd, SPI_IOC_WR_BITS_PER_WORD, &spi_bitsPerWord) < 0) {
		perror("could not set SPI bits per word(WR), ioctl failed");
		close(spifd);
		return 0;
	}
	if (ioctl(spifd, SPI_IOC_RD_BITS_PER_WORD, &spi_bitsPerWord) < 0) {
		perror("could not set SPI bits per word(RD), ioctl failed");
		close(spifd);
		return 0;
	}
	if (ioctl(spifd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed) < 0) {
		perror("could not set SPI speed(wr), ioctl failed");
		close(spifd);
		return 0;
	}
	if (ioctl(spifd, SPI_IOC_RD_MAX_SPEED_HZ, &spi_speed) < 0) {
		perror("could not set SPI speed(rd), ioctl failed");
		close(spifd);
		return 0;
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

