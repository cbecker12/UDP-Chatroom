#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h> 
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h> 
#include <errno.h>
#include "duckchat.h"
#include "raw.h"

#define MAX_INPUT 1024 
#define MAX_CHANNELS 10 

int port_check(const char* port) { 
	for(int i=0; port[i] != '\0'; i++) { 
		if(!isdigit(port[i])) {return 0;} 
	} 
	int port_num = atoi(port); 
	if(port_num < 1024 || port_num > 65535) {return 0;} 
	return 1; // valid port 
} 

int is_channel_joined(char joined_channels[MAX_CHANNELS][CHANNEL_MAX], int channel_count, const char *channel) {
    for (int i = 0; i < channel_count; i++) {
        if (strncmp(joined_channels[i], channel, CHANNEL_MAX) == 0) {
            return 1; // Channel is joined
        }
    }
    return 0;
}

void add_channel(char joined_channels[MAX_CHANNELS][CHANNEL_MAX], int *channel_count, const char *channel) {
    if(*channel_count < MAX_CHANNELS && !is_channel_joined(joined_channels, *channel_count, channel)) {
		strncpy(joined_channels[*channel_count], channel, CHANNEL_MAX);
		(*channel_count)++;
    }
}

void remove_channel(char joined_channels[MAX_CHANNELS][CHANNEL_MAX], int *channel_count, const char *channel) {
    for (int i = 0; i < *channel_count; i++) {
        if (strncmp(joined_channels[i], channel, CHANNEL_MAX) == 0) {
            // Shift remaining channels to fill the gap
            for (int j = i; j < *channel_count - 1; j++) {
                strncpy(joined_channels[j], joined_channels[j + 1], CHANNEL_MAX);
            }
            (*channel_count)--;
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <username>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
	
	if( port_check(argv[2]) == 0 ) { 
		fprintf(stderr, "Error: Port must be between 1024 and 65535.\n"); 
		exit(EXIT_FAILURE); 
	} 
	
	if(strlen(argv[3]) > USERNAME_MAX) { 
		fprintf(stderr, "Error: Username max length is %d characters.\n", USERNAME_MAX); 
		exit(EXIT_FAILURE); 
	} 

    int sockfd;
    struct sockaddr_in server_addr;
    fd_set read_fds;
    char input[MAX_INPUT];
    char *username = argv[3];

    // Initialize socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(argv[2]));

    /*if (inet_pton(AF_INET, gethostbyname(argv[1])->h_addr_list[0], &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        exit(EXIT_FAILURE);
    }*/ 

    // Send login request
    struct request_login login_request;
    memset(&login_request, 0, sizeof(login_request));
    strncpy(login_request.req_username, username, USERNAME_MAX - 1);
	login_request.req_type = REQ_LOGIN; 
    if (sendto(sockfd, &login_request, sizeof(login_request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to send login request");
        exit(EXIT_FAILURE);
    }
    printf("Logged in as %s\n", username);
	
	struct request_join join_request; 
	memset(&join_request, 0, sizeof(join_request)); 
	//join_request.req_type = htonl(REQ_JOIN); 
	join_request.req_type = REQ_JOIN; 
	strncpy(join_request.req_channel, "Common", CHANNEL_MAX); 
	if(sendto(sockfd, &join_request, sizeof(join_request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { 
		perror("Failed to send initial join request to Common"); 
		close(sockfd); 
		return EXIT_FAILURE; 
	} 
	printf("You have joined chatroom: Common\n"); 

    // Set terminal to raw mode
    /* if (raw_mode() < 0) {
        perror("Failed to set terminal to raw mode");
        exit(EXIT_FAILURE);
    } */
	
	char active_channel[CHANNEL_MAX]; 
	strncpy(active_channel, "Common", CHANNEL_MAX);
    printf("Active channel set to: %s\n", active_channel); 	
	
	char joined_channels[MAX_CHANNELS][CHANNEL_MAX]; 
	int channel_count = 0; 
	add_channel(joined_channels, &channel_count, "Common"); 
	
	//printf("> "); 
	//fflush(stdout); 

    while (1) {
		
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sockfd, &read_fds);

        int activity = select(sockfd + 1, &read_fds, NULL, NULL, NULL);
        if (activity < 0 && errno != EINTR) {
            perror("Select error");
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            memset(input, 0, sizeof(input));
            if (fgets(input, sizeof(input), stdin) == NULL) {
                perror("Error reading input");
                break;
            }
			//printf("input = %s\n", input); 
            // Remove newline character
            input[strcspn(input, "\n")] = '\0'; 
			//printf("input = %s\n", input); 
			

            // Command parsing
            if (input[0] == '/') {
                if (strcmp(input, "/exit") == 0) {
                    struct request_logout logout_request;
                    memset(&logout_request, 0, sizeof(logout_request));
					logout_request.req_type = REQ_LOGOUT; 
                    sendto(sockfd, &logout_request, sizeof(logout_request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
                    printf("Exiting...\n");
                    break;
                } 
				else if (strcmp(input, "/list") == 0) {
                    struct request_list list_request;
                    memset(&list_request, 0, sizeof(list_request));
					list_request.req_type = REQ_LIST; 
                    sendto(sockfd, &list_request, sizeof(list_request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
                } 
				else if(strncmp(input, "/join", 5) == 0) { 
					char* channel_name = input + 5; 
					while(*channel_name == ' ') channel_name++; 
					
					if(*channel_name != '\0') { 
						struct request_join join_request; 
						//join_request.req_type = htonl(REQ_JOIN); 
						join_request.req_type = REQ_JOIN; 
						strncpy(join_request.req_channel, channel_name, CHANNEL_MAX); 
						
						if(sendto(sockfd, &join_request, sizeof(join_request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { 
							perror("Failed to send a join request"); 
						} else { 
							add_channel(joined_channels, &channel_count, channel_name);
                            strncpy(active_channel, channel_name, CHANNEL_MAX);
                            printf("Active channel set to: %s\n", active_channel);
						} 
					}
					

					
				}
				else if(strncmp(input, "/who", 4) == 0) { 
					char* channel_name = input + 4; 
					while(*channel_name == ' ') channel_name++; 
					struct request_who who_request; 
					//join_request.req_type = htonl(REQ_JOIN); 
					who_request.req_type = REQ_WHO; 
					strncpy(who_request.req_channel, channel_name, CHANNEL_MAX); 

					if(sendto(sockfd, &who_request, sizeof(who_request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { 
						perror("Failed to send a join request"); 
					} 
				}
				else if(strncmp(input, "/leave", 6) == 0) { 
					char* channel_name = input + 6; 
					while(*channel_name == ' ') channel_name++; 
					
					if(is_channel_joined(joined_channels, channel_count, channel_name)) { 
						struct request_leave leave_request; 
						//join_request.req_type = htonl(REQ_JOIN); 
						leave_request.req_type = REQ_LEAVE; 
						strncpy(leave_request.req_channel, channel_name, CHANNEL_MAX); 

						if(sendto(sockfd, &leave_request, sizeof(leave_request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { 
							perror("Failed to send a join request"); 
						} else { 
							remove_channel(joined_channels, &channel_count, channel_name); 
							if (strcmp(active_channel, channel_name) == 0) {
									strncpy(active_channel, "Common", CHANNEL_MAX);
									printf("You left %s. Active channel set to: Common\n", channel_name);
							}

						} 
					} else { 
						printf("Error: You are not in channel %s\n", channel_name); 
					} 
				}
				else if(strncmp(input, "/switch", 7) == 0) { 
					char* channel_name = input + 7; 
					// check joined channels 
					//active_channel = channel_name;i
					while(*channel_name == ' ') channel_name++; 	
					
					if(is_channel_joined(joined_channels, channel_count, channel_name)) {
						strncpy(active_channel, channel_name, CHANNEL_MAX-1);
						active_channel[CHANNEL_MAX-1] = '\0'; 	
						printf("Active channel set to: %s\n", active_channel);
					} else { 
						printf("Error: You must join %s before switching into it.\n", channel_name); 
					} 
				} 
                // Additional commands can be added here
                else {
                    printf("Unknown command: %s\n", input);
                }
            } 
            else {
                struct request_say say_request;
                memset(&say_request, 0, sizeof(say_request));
				say_request.req_type = REQ_SAY; 
                strncpy(say_request.req_text, input, SAY_MAX - 1);
				strncpy(say_request.req_channel, active_channel, CHANNEL_MAX - 1);

                if (sendto(sockfd, &say_request, sizeof(say_request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                    perror("Failed to send message");
                }
            }
		}

        if (FD_ISSET(sockfd, &read_fds)) {
            /*struct text_say message;
            socklen_t addr_len = sizeof(server_addr);
            ssize_t bytes_received = recvfrom(sockfd, &message, sizeof(message), 0, (struct sockaddr*)&server_addr, &addr_len);

            if (bytes_received < 0) {
                perror("Failed to receive message");
                break;
            }

            printf("[%s]: %s\n", message.txt_username, message.txt_text);*/ 
			char message[1024]; 
			socklen_t addr_len = sizeof(server_addr);
            ssize_t bytes_received = recvfrom(sockfd, &message, sizeof(message), 0, (struct sockaddr*)&server_addr, &addr_len);

            if (bytes_received < 0) {
                perror("Failed to receive message");
                break;
            }
			
			struct text* txt = (struct text*)message; 
			//printf("server msg type: %d\n", txt->txt_type); 
			
			switch(txt->txt_type) { 
				case TXT_SAY: {
					struct text_say *msg_say = (struct text_say *)message; 
					printf("[%s][%s]: %s\n", msg_say->txt_channel, msg_say->txt_username, msg_say->txt_text);
					break;
				}
				case TXT_ERROR: {
					struct text_error *msg_error = (struct text_error *)message;
					fprintf(stderr, "Error: %s\n", msg_error->txt_error);
					break;
				}
				case TXT_LIST: {
					struct text_list *msg_list = (struct text_list *)message;
					printf("There are %d channel(s):\n", msg_list->txt_nchannels);
					for(int i=0; i<msg_list->txt_nchannels; i++) { 
						printf("\t - %s\n", msg_list->txt_channels[i].ch_channel); 
					} 
					break;
				}
				case TXT_WHO: {
					struct text_who *msg_who = (struct text_who *)message;
					printf("There are %d users in %s.\n", msg_who->txt_nusernames, msg_who->txt_channel);
					for(int i=0; i<msg_who->txt_nusernames; i++) { 
						printf("\t - %s\n", msg_who->txt_users[i].us_username); 
					} 
					break;
				}
				default:
					fprintf(stderr, "Unknown message type received: %d\n", txt->txt_type);
					break;
			} 
        }
    }

    //cooked_mode(); // Reset terminal mode
    close(sockfd);
    return 0;
}
