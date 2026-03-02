#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> 
#include <fcntl.h> 
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <ctype.h>
#include "duckchat.h"

#define MAX_USERS 100
#define MAX_CHANNELS 100
#define MAX_NEIGHBORS 10 // adjust? 
#define MAX_UNIQUE_IDS 1000

typedef struct {
    char username[USERNAME_MAX];
    char channels[MAX_CHANNELS][CHANNEL_MAX];
    int channel_count;
    struct sockaddr_in address;
    int logged_in;
} User;

typedef struct {
    User users[MAX_USERS];
    int user_count;
	char active_channels[MAX_CHANNELS][CHANNEL_MAX];
    int channel_count; 
} Server;

typedef struct { 
	struct sockaddr_in address; 
} Neighbor; 

typedef struct {
    char channel[CHANNEL_MAX];
    int neighbor_indices[MAX_NEIGHBORS];
    int neighbor_count;
} ChannelRouting;

ChannelRouting channel_routing[MAX_CHANNELS];
int routing_count = 0;

Server server; 

Neighbor neighbors[MAX_NEIGHBORS]; 
int neighbor_count = 0; 

char resolved_ip[INET_ADDRSTRLEN]; 
int port; 

uint64_t seen_unique_ids[MAX_UNIQUE_IDS];
int seen_unique_count = 0;
int seen_unique_start = 0; 

// Function to seed the random number generator from /dev/urandom
void seed_random() {
    unsigned int seed;
    int urandom = open("/dev/urandom", O_RDONLY);
    if (urandom < 0) {
        perror("Failed to open /dev/urandom");
        exit(EXIT_FAILURE);
    }
    if (read(urandom, &seed, sizeof(seed)) != sizeof(seed)) {
        perror("Failed to read from /dev/urandom");
        close(urandom);
        exit(EXIT_FAILURE);
    }
    close(urandom);
    srand(seed); // Seed the random number generator
}

void print_routing_table() {
    printf("Routing table for server %s:%d\n", resolved_ip, port);

    for (int i = 0; i < routing_count; i++) {
        ChannelRouting *routing = &channel_routing[i];
        printf("Channel: %s\n", routing->channel);

        for (int j = 0; j < routing->neighbor_count; j++) {
            int neighbor_index = routing->neighbor_indices[j];
            char neighbor_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &neighbors[neighbor_index].address.sin_addr, neighbor_ip, INET_ADDRSTRLEN);
            int neighbor_port = ntohs(neighbors[neighbor_index].address.sin_port);

            printf("    Neighbor: %s:%d\n", neighbor_ip, neighbor_port);
        }
    }

    if (routing_count == 0) {
        printf("    (No channels in the routing table)\n");
    }
}

int is_unique_id_seen(uint64_t unique_id) {
    for (int i = 0; i < seen_unique_count; i++) {
        int index = (seen_unique_start + i) % MAX_UNIQUE_IDS;
        if (seen_unique_ids[index] == unique_id) {
            return 1; // Already seen
        }
    }
    return 0;
}

void add_unique_id(uint64_t unique_id) {
    if (seen_unique_count < MAX_UNIQUE_IDS) {
        seen_unique_ids[(seen_unique_start + seen_unique_count) % MAX_UNIQUE_IDS] = unique_id;
        seen_unique_count++;
    } else {
        seen_unique_ids[seen_unique_start] = unique_id;
        seen_unique_start = (seen_unique_start + 1) % MAX_UNIQUE_IDS;
    }
} 

void handle_s2s_leave(int sockfd, struct sockaddr_in* neighbor_addr, struct s2s_leave* leave_request) {
	if(sockfd) {;} 
    const char* channel = leave_request->req_channel;
    int neighbor_index = -1;

    // Validate neighbor
    for (int i = 0; i < neighbor_count; i++) {
        if (memcmp(&neighbors[i].address, neighbor_addr, sizeof(*neighbor_addr)) == 0) {
            neighbor_index = i;
            break;
        }
    }
    if (neighbor_index == -1) return; // Validation failed, drop message

    // Find the channel in RT 
    int channel_index = -1;
    for (int i = 0; i < routing_count; i++) {
        if (strcmp(channel_routing[i].channel, channel) == 0) {
            channel_index = i;
            break;
        }
    }
    if (channel_index == -1) return; // Channel not found, drop message

    // Remove neighbor from the channel's RT
    ChannelRouting* routing = &channel_routing[channel_index];
    int neighbor_found = 0;
    for (int i = 0; i < routing->neighbor_count; i++) {
        if (routing->neighbor_indices[i] == neighbor_index) {
            // Shift the neighbors list left to remove the neighbor
            for (int j = i; j < routing->neighbor_count - 1; j++) {
                routing->neighbor_indices[j] = routing->neighbor_indices[j + 1];
            }
            routing->neighbor_count--;
            neighbor_found = 1;
            break;
        }
    }

    if (!neighbor_found) return; // Neighbor wasn't subscribed to this channel

    // Check if the channel is still used locally or by other neighbors
    int channel_in_use = routing->neighbor_count > 0;
    if (!channel_in_use) {
        // Check if any local users are in this channel
        for (int i = 0; i < server.user_count; i++) {
            for (int j = 0; j < server.users[i].channel_count; j++) {
                if (strcmp(server.users[i].channels[j], channel) == 0) {
                    channel_in_use = 1;
                    break;
                }
            }
            if (channel_in_use) break;
        }
    }

    // If the channel is no longer in use, remove it from RT 
    if (!channel_in_use) {
        for (int i = channel_index; i < routing_count - 1; i++) {
            channel_routing[i] = channel_routing[i + 1];
        }
        routing_count--;
    }
	
	/* Notify the original sender of the leave
    sendto(sockfd, leave_request, sizeof(*leave_request), 0,
           (struct sockaddr*)neighbor_addr, sizeof(*neighbor_addr));
    char neighbor_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &neighbor_addr->sin_addr, neighbor_ip, INET_ADDRSTRLEN);
    int neighbor_port = ntohs(neighbor_addr->sin_port);
    printf("%s:%d %s:%d send S2S Leave %s\n", resolved_ip, port, neighbor_ip, neighbor_port, channel); */ 

    /* Forward the S2S_LEAVE message to other neighbors
    for (int i = 0; i < neighbor_count; i++) {
        if (i != neighbor_index) {
            sendto(sockfd, leave_request, sizeof(*leave_request), 0, 
                   (struct sockaddr*)&neighbors[i].address, sizeof(neighbors[i].address));

            char neighbor_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &neighbors[i].address.sin_addr, neighbor_ip, INET_ADDRSTRLEN);
            int neighbor_port = ntohs(neighbors[i].address.sin_port);

            printf("%s:%d %s:%d send S2S Leave %s\n", resolved_ip, port,
                   neighbor_ip, neighbor_port, leave_request->req_channel);
        }
    } */ 
}

void handle_s2s_say(int sockfd, struct sockaddr_in* neighbor_addr, struct s2s_say* say_request) {
    // Check if unique_id already processed
    if (is_unique_id_seen(say_request->unique_id)) {
        // Send S2S_LEAVE to sender
        struct s2s_leave leave_message;
        leave_message.req_type = S2S_LEAVE;
        strncpy(leave_message.req_channel, say_request->req_channel, CHANNEL_MAX - 1);
		
		char sender_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &neighbor_addr->sin_addr, sender_ip, INET_ADDRSTRLEN);
        int sender_port = ntohs(neighbor_addr->sin_port);
        printf("%s:%d %s:%d send S2S Leave %s\n", resolved_ip, port, sender_ip, sender_port, leave_message.req_channel);
        
		sendto(sockfd, &leave_message, sizeof(leave_message), 0, 
               (struct sockaddr*)neighbor_addr, sizeof(*neighbor_addr)); 
        return;
    }
    add_unique_id(say_request->unique_id);

    const char* channel = say_request->req_channel;

    // Verify the channel exists in the routing table
    int channel_index = -1;
    for (int i = 0; i < routing_count; i++) {
        if (strcmp(channel_routing[i].channel, channel) == 0) {
            channel_index = i;
            break;
        }
    }

    if (channel_index == -1) {
        printf("S2S_SAY dropped: Unknown channel %s\n", channel);
        return; // Channel not found -- maybe send leave? 
    }

    ChannelRouting* routing = &channel_routing[channel_index];

    // Deliver the message to local users
    struct text_say message;
    message.txt_type = TXT_SAY;
    strncpy(message.txt_channel, say_request->req_channel, CHANNEL_MAX - 1);
    strncpy(message.txt_username, say_request->req_username, USERNAME_MAX - 1);
    strncpy(message.txt_text, say_request->req_text, SAY_MAX - 1);

    for (int i = 0; i < server.user_count; i++) {
        User* user = &server.users[i];
        for (int j = 0; j < user->channel_count; j++) {
            if (strcmp(user->channels[j], channel) == 0) {
                sendto(sockfd, &message, sizeof(message), 0, 
                       (struct sockaddr*)&user->address, sizeof(user->address));
                break;
            }
        }
    }

    // Forward the message to all neighbors except the sender
	int forwarded = 0; 
    for (int i = 0; i < routing->neighbor_count; i++) {
        int neighbor_index = routing->neighbor_indices[i];
		struct sockaddr_in *neighbor_addr2 = &neighbors[neighbor_index].address;
		
		// Convert neighbor's address to string
		char client_ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &neighbor_addr2->sin_addr, client_ip, INET_ADDRSTRLEN);
		int client_port = ntohs(neighbor_addr2->sin_port);
		
        if (memcmp(&neighbors[neighbor_index].address, neighbor_addr, sizeof(*neighbor_addr)) != 0) {
            sendto(sockfd, say_request, sizeof(*say_request), 0, 
                   (struct sockaddr*)&neighbors[neighbor_index].address, sizeof(neighbors[neighbor_index].address));
            printf("%s:%d %s:%d send S2S Say %s %s \"%s\"\n", resolved_ip, port, client_ip, client_port,
				say_request->req_username, say_request->req_channel, say_request->req_text);
			forwarded = 1; 
        }
    }
	
	// If no neighbors were valid to forward the message, check local clients before sending S2S_LEAVE
    if (!forwarded) {
        int has_local_clients = 0;
        for (int i = 0; i < server.user_count; i++) {
            for (int j = 0; j < server.users[i].channel_count; j++) {
                if (strcmp(server.users[i].channels[j], channel) == 0) {
                    has_local_clients = 1;
                    break;
                }
            }
            if (has_local_clients) break;
        }

        if (!has_local_clients) {
            // No neighbors or local clients, send S2S_LEAVE to the sender
            struct s2s_leave leave_message;
            leave_message.req_type = S2S_LEAVE;
            strncpy(leave_message.req_channel, say_request->req_channel, CHANNEL_MAX - 1);

            sendto(sockfd, &leave_message, sizeof(leave_message), 0,
                   (struct sockaddr*)neighbor_addr, sizeof(*neighbor_addr));

            char sender_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &neighbor_addr->sin_addr, sender_ip, INET_ADDRSTRLEN);
            int sender_port = ntohs(neighbor_addr->sin_port);

            printf("%s:%d %s:%d send S2S Leave %s\n", resolved_ip, port, sender_ip, sender_port, leave_message.req_channel);
        }
    }
} 

void handle_s2s_join(int sockfd, struct sockaddr_in* neighbor_addr, struct s2s_join* join_request) {
    const char* channel = join_request->req_channel;
    int neighbor_index = -1;

    // Find neighbor
    for (int i = 0; i < neighbor_count; i++) {
        if (memcmp(&neighbors[i].address, neighbor_addr, sizeof(*neighbor_addr)) == 0) {
            neighbor_index = i;
            break;
        }
    }
    if (neighbor_index == -1) return; // Unknown neighbor, drop message

    // Check if the channel is in the routing table
    int channel_index = -1;
    for (int i = 0; i < routing_count; i++) {
        if (strcmp(channel_routing[i].channel, channel) == 0) {
            channel_index = i;
            break;
        }
    }

    // Add channel to routing table if not present
    if (channel_index == -1 && routing_count < MAX_CHANNELS) {
        channel_index = routing_count++;
        strncpy(channel_routing[channel_index].channel, channel, CHANNEL_MAX - 1);
        channel_routing[channel_index].neighbor_count = 0;
    }

    // Add the neighbor to the channel's routing table
    ChannelRouting *routing = &channel_routing[channel_index];
    for (int i = 0; i < routing->neighbor_count; i++) {
        if (routing->neighbor_indices[i] == neighbor_index) {
            return; // Neighbor already in routing table
        }
    }
    if (routing->neighbor_count < MAX_NEIGHBORS) {
        routing->neighbor_indices[routing->neighbor_count++] = neighbor_index;
    }

    // Forward join to other neighbors
    for (int i = 0; i < neighbor_count; i++) {
        if (i != neighbor_index) {
            sendto(sockfd, join_request, sizeof(*join_request), 0, 
                   (struct sockaddr*)&neighbors[i].address, sizeof(neighbors[i].address));
            printf("%s:%d %s:%d send S2S Join %s\n", resolved_ip, port,
                   inet_ntoa(neighbors[i].address.sin_addr), ntohs(neighbors[i].address.sin_port),
                   join_request->req_channel);

            // Add neighbor to routing table
            if (routing->neighbor_count < MAX_NEIGHBORS) {
                routing->neighbor_indices[routing->neighbor_count++] = i;
            }
        }
    }
	//if(port == 4000) {print_routing_table();} 
}


int validate_ip(const char* ip) {
    struct sockaddr_in sa;
    return inet_pton(AF_INET, ip, &(sa.sin_addr)) != 0;
} 

int port_check(const char* port) {
    for (int i = 0; port[i] != '\0'; i++) {
        if (!isdigit(port[i])) return 0;
    }
    int port_num = atoi(port);
    return (port_num >= 1024 && port_num <= 65535);
} 

void add_neighbor(const char* ip, int port) {
    if (neighbor_count >= MAX_NEIGHBORS) {
        fprintf(stderr, "Error: Too many neighbors specified. Maximum allowed is %d.\n", MAX_NEIGHBORS);
        exit(EXIT_FAILURE);
    }

    Neighbor* neighbor = &neighbors[neighbor_count++];
    memset(&neighbor->address, 0, sizeof(neighbor->address));
    neighbor->address.sin_family = AF_INET;
    neighbor->address.sin_port = htons(port);
	
	struct hostent* hserver; 
	if ((hserver = gethostbyname(ip)) == NULL) {
		fprintf(stderr, "Error: Invalid neighbor host %s\n", ip);
        exit(EXIT_FAILURE);
    }
	//char resolved_ip[INET_ADDRSTRLEN];
    //inet_ntop(AF_INET, hserver->h_addr_list[0], resolved_ip, INET_ADDRSTRLEN);
    //printf("Neighbor host resolved to IP: %s\n", resolved_ip);
	memcpy(&neighbor->address.sin_addr.s_addr, hserver->h_addr_list[0], hserver->h_length); 

    /*if (inet_pton(AF_INET, ip, &neighbor->address.sin_addr) <= 0) {
        fprintf(stderr, "Error: Invalid IP address %s\n", ip);
        exit(EXIT_FAILURE);
    }*/ 
}

int find_user(const char* username) {
    for (int i = 0; i < server.user_count; i++) {
        if (strcmp(server.users[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
} 

int find_channel(const char* channel) {
    for (int i = 0; i < server.channel_count; i++) {
        if (strcmp(server.active_channels[i], channel) == 0) {
            return i;
        }
    }
    return -1;
}

void add_channel(const char* channel) {
    if (find_channel(channel) == -1 && server.channel_count < MAX_CHANNELS) {
        strncpy(server.active_channels[server.channel_count++], channel, CHANNEL_MAX - 1);
    }
}

int is_user_in_channel(User* user, const char* channel_name) {
    for (int i = 0; i < user->channel_count; i++) {
        if (strcmp(user->channels[i], channel_name) == 0) {
            return i;  // Return the index of the channel if found
        }
    }
    return -1;  // Return -1 if the user is not in the channel
} 

void send_error(int sockfd, struct sockaddr_in* client_addr, const char* error_msg) {
    struct text_error error;
    error.txt_type = TXT_ERROR;
    strncpy(error.txt_error, error_msg, SAY_MAX - 1);
    sendto(sockfd, &error, sizeof(error), 0, (struct sockaddr *)client_addr, sizeof(*client_addr));
}

void handle_login(int sockfd, struct sockaddr_in* client_addr, struct request_login* login_request) {
    if (find_user(login_request->req_username) == -1 && server.user_count < MAX_USERS) {
        User *user = &server.users[server.user_count++];
        strncpy(user->username, login_request->req_username, USERNAME_MAX - 1);
        user->address = *client_addr;
        user->logged_in = 1;
        printf("server: %s logs in\n", user->username);
    } else {
        send_error(sockfd, client_addr, "Login failed: Username already taken or max users reached.");
    }
}

void handle_join(int sockfd, int user_index, struct request_join* join_request) {
    User *user = &server.users[user_index];
	
	// Check if user is in channel already 
    for (int i = 0; i < user->channel_count; i++) {
        if (strcmp(user->channels[i], join_request->req_channel) == 0) {
            return; // They are 
        }
    }
    if (user->channel_count < MAX_CHANNELS) {
		// Add channel to user's list 
        strncpy(user->channels[user->channel_count++], join_request->req_channel, CHANNEL_MAX - 1);
        //add_channel(join_request->req_channel); 
		printf("server: %s joins channel %s\n", user->username, join_request->req_channel);
		
		int channel_index = find_channel(join_request->req_channel); 
		
		if(channel_index == -1) { 
			add_channel(join_request->req_channel); 
			
			// Add channel to routing table
            if (routing_count < MAX_CHANNELS) {
                channel_index = routing_count++;
                strncpy(channel_routing[channel_index].channel, join_request->req_channel, CHANNEL_MAX - 1);
                channel_routing[channel_index].neighbor_count = 0;
            }
			
			// Send S2S_JOIN to neighbors 
			struct s2s_join join_msg; 
			join_msg.req_type = S2S_JOIN; 
			strncpy(join_msg.req_channel, join_request->req_channel, CHANNEL_MAX - 1); 
			for(int i=0; i<neighbor_count; i++) { 
				char client_ip[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &neighbors[i].address.sin_addr, client_ip, INET_ADDRSTRLEN);
				int client_port = ntohs(neighbors[i].address.sin_port);
				printf("%s:%d %s:%d send S2S Join %s\n", resolved_ip, port, 
					client_ip, client_port, join_request->req_channel); 
				sendto(sockfd, &join_msg, sizeof(join_msg), 0, 
					(struct sockaddr*)&neighbors[i].address, sizeof(neighbors[i].address)); 
				//printf("Sent S2S_JOIN to neighbor %d for channel %s\n", i, join_request->req_channel); 
				
					
				// Add neighbor to routing table
                ChannelRouting* routing = &channel_routing[channel_index];
                if (routing->neighbor_count < MAX_NEIGHBORS) {
                    routing->neighbor_indices[routing->neighbor_count++] = i;
                }
			} 
		} 
		//if(port == 4005) {print_routing_table();} 
    } else {
        send_error(sockfd, &user->address, "Join failed: Max channels reached.");
    }
}

void handle_say(int sockfd, int user_index, struct request_say* say_request) {
    User* user = &server.users[user_index];
    int in_channel = 0;
    for (int i = 0; i < user->channel_count; i++) {
        if (strcmp(user->channels[i], say_request->req_channel) == 0) {
            in_channel = 1;
            break;
        }
    }
    if (!in_channel) {
        send_error(sockfd, &user->address, "Error: Not in specified channel.");
        return;
    }

    struct text_say message;
    message.txt_type = TXT_SAY;
    strncpy(message.txt_username, user->username, USERNAME_MAX - 1);
    strncpy(message.txt_channel, say_request->req_channel, CHANNEL_MAX - 1);
    strncpy(message.txt_text, say_request->req_text, SAY_MAX - 1);

    for (int i = 0; i < server.user_count; i++) {
        User *other_user = &server.users[i];
        for (int j = 0; j < other_user->channel_count; j++) {
            if (strcmp(other_user->channels[j], say_request->req_channel) == 0) {
                sendto(sockfd, &message, sizeof(message), 0, (struct sockaddr *)&other_user->address, sizeof(other_user->address));
                break;
            }
        }
    }
    printf("server: %s sends say message in %s\n", user->username, say_request->req_channel); 
	
	// Forward the message as S2S_SAY to all neighbors in the routing table
    int channel_index = -1;
    for (int i = 0; i < routing_count; i++) {
        if (strcmp(channel_routing[i].channel, say_request->req_channel) == 0) {
            channel_index = i;
            break;
        }
    }

    if (channel_index != -1) {
        ChannelRouting* routing = &channel_routing[channel_index];

        struct s2s_say s2s_message;
        s2s_message.req_type = S2S_SAY;
        s2s_message.unique_id = (uint64_t)rand(); // Random unique ID
		//printf("random unique ID gen: %lu\n", s2s_message.unique_id); 
        strncpy(s2s_message.req_channel, say_request->req_channel, CHANNEL_MAX - 1);
        strncpy(s2s_message.req_username, user->username, USERNAME_MAX - 1);
        strncpy(s2s_message.req_text, say_request->req_text, SAY_MAX - 1);

		char client_ip[INET_ADDRSTRLEN]; 
        for (int i = 0; i < routing->neighbor_count; i++) {
			int neighbor_index = routing->neighbor_indices[i];
			struct sockaddr_in *neighbor_addr = &neighbors[neighbor_index].address;

			// Convert neighbor's address to string
			inet_ntop(AF_INET, &neighbor_addr->sin_addr, client_ip, INET_ADDRSTRLEN);
			int client_port = ntohs(neighbor_addr->sin_port);
			
            sendto(sockfd, &s2s_message, sizeof(s2s_message), 0, 
                   (struct sockaddr*)&neighbors[routing->neighbor_indices[i]].address, 
                   sizeof(neighbors[routing->neighbor_indices[i]].address));
            printf("%s:%d %s:%d send S2S Say %s %s \"%s\"\n", resolved_ip, port, client_ip, client_port, 
				user->username, say_request->req_channel, say_request->req_text);
        }
    }
} 

void handle_list(int sockfd, int user_index) { 
	User* user = &server.users[user_index]; 
	printf("server: %s requests a list of channels\n", user->username); 
	
	// Prepare the list of active channels to send back
    struct {
        text_t txt_type;
        int txt_nchannels;
        struct channel_info txt_channels[MAX_CHANNELS];
    } response;

    response.txt_type = TXT_LIST;
    response.txt_nchannels = server.channel_count;

    for (int i = 0; i < server.channel_count; i++) {
        strncpy(response.txt_channels[i].ch_channel, server.active_channels[i], CHANNEL_MAX - 1);
    }

    // Send the list back to the user
    sendto(sockfd, &response, sizeof(response), 0, (struct sockaddr *)&user->address, sizeof(user->address));
} 

void handle_who(int sockfd, int user_index, struct request_who* who_request) { 
	User* requesting_user = &server.users[user_index];
    const char *channel_name = who_request->req_channel;
    
    // Check if the channel exists
    if (find_channel(channel_name) == -1) {
        send_error(sockfd, &requesting_user->address, "Error: Channel does not exist.");
        return;
    }

    // Count the number of users in the requested channel
    int user_count = 0;
    for (int i = 0; i < server.user_count; i++) {
        if (is_user_in_channel(&server.users[i], channel_name) != -1) {
            user_count++;
        }
    }

    // Allocate memory dynamically for the response
    size_t response_size = sizeof(struct text_who) + user_count * sizeof(struct user_info);
    struct text_who *response = (struct text_who *)malloc(response_size);
    if (!response) {
        perror("Memory allocation failed");
        return;
    }

    // Populate the response struct
    response->txt_type = TXT_WHO;
    response->txt_nusernames = user_count;
    strncpy(response->txt_channel, channel_name, CHANNEL_MAX - 1);

    // Add each user in the channel to the response
    int idx = 0;
    for (int i = 0; i < server.user_count; i++) {
        if (is_user_in_channel(&server.users[i], channel_name) != -1) {
            strncpy(response->txt_users[idx++].us_username, server.users[i].username, USERNAME_MAX - 1);
        }
    }

    // Send the response back to the requesting user
    sendto(sockfd, response, response_size, 0, (struct sockaddr *)&requesting_user->address, sizeof(requesting_user->address));

    // Free the allocated memory
    free(response);

    printf("server: %s requests a list of users in channel %s\n", requesting_user->username, channel_name);
} 

void handle_leave(int sockfd, int user_index, struct request_leave* leave_request) { 
	User* user = &server.users[user_index];
    int channel_index = is_user_in_channel(user, leave_request->req_channel);

    if (channel_index == -1) {
        // User is not in the specified channel
        send_error(sockfd, &user->address, "Error: Not in specified channel.");
        return;
    }

    // Remove the channel from the user's list
    for (int i = channel_index; i < user->channel_count - 1; i++) {
        strncpy(user->channels[i], user->channels[i + 1], CHANNEL_MAX);
    }
    user->channel_count--;

    // Check if the channel is empty (i.e., no users are in it)
    int channel_in_use = 0;
    for (int i = 0; i < server.user_count; i++) {
        if (is_user_in_channel(&server.users[i], leave_request->req_channel) != -1) {
            channel_in_use = 1;
            break;
        }
    }

    // If the channel is not in use, remove it from the active channels list
    if (!channel_in_use) {
        int global_channel_index = find_channel(leave_request->req_channel);
        if (global_channel_index != -1) {
            for (int i = global_channel_index; i < server.channel_count - 1; i++) {
                strncpy(server.active_channels[i], server.active_channels[i + 1], CHANNEL_MAX);
            }
            server.channel_count--;
        }
    }

    printf("server: %s leaves channel %s\n", user->username, leave_request->req_channel);
} 

void handle_logout(int user_index) {
    User *user = &server.users[user_index];
    printf("server: %s logs out\n", user->username);
    server.users[user_index] = server.users[--server.user_count];
}

void handle_invalid(int sockfd, struct sockaddr_in *client_addr) {
    send_error(sockfd, client_addr, "Invalid request or not logged in.");
}

int main(int argc, char *argv[]) {
    if (argc < 3 || (argc-3) % 2 != 0 || !port_check(argv[2])) {
        fprintf(stderr, "Usage: %s <host> <port> [<neighbor_ip> <neighbor_port>]...\n", argv[0]);
        exit(EXIT_FAILURE);
    } 
	seed_random(); 
	// Identify and resolve server host, port 
	const char* host = argv[1]; 
	port = atoi(argv[2]); 
	struct hostent* hserver;
    if ((hserver = gethostbyname(host)) == NULL) {
		fprintf(stderr, "Error: Invalid host %s\n", host);
        exit(EXIT_FAILURE);
    }
	//char resolved_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, hserver->h_addr_list[0], resolved_ip, INET_ADDRSTRLEN);
    //printf("Host resolved to IP: %s\n", resolved_ip);
	
	// Create socket struct info 
	struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
	memcpy(&server_addr.sin_addr.s_addr, hserver->h_addr_list[0], hserver->h_length); 
	
	// Parse and validate neighbor arguments
    for (int i = 3; i < argc; i += 2) {
        const char* neighbor_ip = argv[i];
        int neighbor_port = atoi(argv[i + 1]);

        if (!port_check(argv[i + 1])) {
            fprintf(stderr, "Error: Invalid neighbor port %s\n", argv[i + 1]);
            exit(EXIT_FAILURE);
        }
        add_neighbor(neighbor_ip, neighbor_port);
    }

    // Display server and neighbor information
    printf("Server started at %s:%d\n", resolved_ip, port);
    for (int i = 0; i < neighbor_count; i++) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &neighbors[i].address.sin_addr, ip, INET_ADDRSTRLEN);
        printf("%s:%d Neighbor %d: %s:%d\n", resolved_ip, port, i + 1, ip, ntohs(neighbors[i].address.sin_port));
    }
	
	// Create and bind server socket 
    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
	}
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
	
	struct sockaddr_in client_addr;

    while (1) {
        char buffer[1024];
        socklen_t addr_len = sizeof(client_addr);
        int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &addr_len);

        if (bytes_received < 0) {
            perror("Failed to receive message");
            continue;
        }

        struct request* req = (struct request *)buffer;
		char client_ip[INET_ADDRSTRLEN];
		struct sockaddr_in *cli = (struct sockaddr_in *)&client_addr;
		inet_ntop(AF_INET, &cli->sin_addr, client_ip, INET_ADDRSTRLEN);
		int client_port = ntohs(cli->sin_port);
		
		if(req->req_type >= S2S_JOIN) { // S2S checks 
			switch(req->req_type) { 
				case S2S_JOIN: { 
					struct s2s_join* join_request = (struct s2s_join*)req; 
					printf("%s:%d %s:%d recv S2S Join %s\n", resolved_ip, port, client_ip, client_port, join_request->req_channel); 
					handle_s2s_join(sockfd, &client_addr, join_request); 
					break; 
				} 
				case S2S_SAY: 
					struct s2s_say* say_request = (struct s2s_say*)req; 
					printf("%s:%d %s:%d recv S2S Say %s %s \"%s\"\n", resolved_ip, port, client_ip, client_port, 
						say_request->req_username, say_request->req_channel, say_request->req_text); 
					handle_s2s_say(sockfd, &client_addr, (struct s2s_say*)req); 
					break; 
				case S2S_LEAVE: {
					struct s2s_leave* leave_request = (struct s2s_leave*)req;
					printf("%s:%d %s:%d recv S2S Leave %s\n", resolved_ip, port, client_ip, client_port, leave_request->req_channel);
					handle_s2s_leave(sockfd, &client_addr, leave_request);
					break;
				}

				default: 
					fprintf(stderr, "Unknown S2S message type: %d\n", req->req_type); 
					break; 
			} 
		}
		else { // Client checks 
		
			int user_index = -1;
			for (int i = 0; i < server.user_count; i++) {
				if (memcmp(&server.users[i].address, &client_addr, sizeof(client_addr)) == 0) {
					user_index = i;
					break;
				}
			}

			if (req->req_type == REQ_LOGIN) {
				handle_login(sockfd, &client_addr, (struct request_login *)req);
			} else if (user_index == -1 || !server.users[user_index].logged_in) {
				handle_invalid(sockfd, &client_addr);
			} else {
				switch (req->req_type) {
					case REQ_JOIN:
						handle_join(sockfd, user_index, (struct request_join *)req);
						break;
					case REQ_SAY:
						handle_say(sockfd, user_index, (struct request_say *)req);
						break;
					case REQ_LIST: 
						handle_list(sockfd, user_index); 
						break; 
					case REQ_WHO: 
						handle_who(sockfd, user_index, (struct request_who *)req); 
						break; 
					case REQ_LEAVE: 
						handle_leave(sockfd, user_index, (struct request_leave *)req); 
						break; 
					case REQ_LOGOUT:
						handle_logout(user_index);
						break;
					default:
						handle_invalid(sockfd, &client_addr);
						break;
				}
			}
		}
    }

    close(sockfd);
    return 0;
}
