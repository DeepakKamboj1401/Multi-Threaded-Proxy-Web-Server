#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define MAX_BYTES 4096    // Max allowed size of Request/Response
#define MAX_CLIENTS 400     // Max number of Client requests served at a time
#define MAX_SIZE 200 * (1 << 20)     // Size of the cache
#define MAX_ELEMENT_SIZE 10 * (1 << 20)     // Max size of an element in cache

typedef struct Cache_element Cache_element;

struct Cache_element {
    char *Data;
    int Length;
    char *URL;
    time_t LRU_time_track;
    Cache_element *Next;
};

Cache_element* Find(char *URL);
int Add_Cache_Element(char *Data, int size, char *URL);
void Remove_Cache_Element();

int Port_Number = 8080; 
// i.e Our Proxy Server will work on the Port 8080.

// To maintain the connections between the servers, we need to use Socket Programming.
int Proxy_Socket_ID;

/*
    Since our Proxy Server is Multi-Threaded, so for each Client Request we need to open a new socket.
    In order to do this, we need Threads (Workers).
    Number of Threads == Number of Clients. i.e in every thread, we will have one socket.
    So we will make an array of threads.
*/

pthread_t Thread_ID[MAX_CLIENTS];

sem_t Semaphore; // Can we form any further threads or not ?

/*
    Since our Proxy Server is multi-threaded so Multiple clients can request simaltaneously and each request
    will go to the individual socket which is managed by an individual thread, therefore there will be 
    multiple threads and for them LRU Cache will be a shared resource which can lead to Race situation.
    Race Condition is handled by mutex lock.
*/

pthread_mutex_t Lock;

Cache_element *Head;
int Cache_Size;

// http://localhost:8080/https://www.cs.princeton.edu/


int sendErrorMessage(int socket, int status_code)
{
	char String[1024];
	char current_Time[50];
	time_t now = time(0);

	struct tm data = *gmtime(&now);
	strftime(current_Time, sizeof(current_Time),"%a, %d %b %Y %H:%M:%S %Z", &data);

	switch(status_code)
	{
		case 400: snprintf(String, sizeof(String), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", current_Time);
				  printf("400 Bad Request\n");
				  send(socket, String, strlen(String), 0);
				  break;

		case 403: snprintf(String, sizeof(String), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", current_Time);
				  printf("403 Forbidden\n");
				  send(socket, String, strlen(String), 0);
				  break;

		case 404: snprintf(String, sizeof(String), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", current_Time);
				  printf("404 Not Found\n");
				  send(socket, String, strlen(String), 0);
				  break;

		case 500: snprintf(String, sizeof(String), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", current_Time);
				  //printf("500 Internal Server Error\n");
				  send(socket, String, strlen(String), 0);
				  break;

		case 501: snprintf(String, sizeof(String), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", current_Time);
				  printf("501 Not Implemented\n");
				  send(socket, String, strlen(String), 0);
				  break;

		case 505: snprintf(String, sizeof(String), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", current_Time);
				  printf("505 HTTP Version Not Supported\n");
				  send(socket, String, strlen(String), 0);
				  break;

		default:  return -1;

	}
	return 1;
}

int connectRemoteServer(char* Host_Address, int Port_Number)
{
	// Creating Socket for remote server 

	int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

	if(remoteSocket < 0){
		printf("Error in Creating Socket.\n");
		return -1;
	}
	
	// Get Host by the name or IP Address provided

	struct hostent *host = gethostbyname(Host_Address);	
	if(host == NULL)
	{
		fprintf(stderr, "No such Host exists.\n");	
		return -1;
	}

	// Inserts IP Address and Port Number of Host in struct `Server_Address`

	struct sockaddr_in Server_Address;

	bzero((char*)&Server_Address, sizeof(Server_Address));
	Server_Address.sin_family = AF_INET;
	Server_Address.sin_port = htons(Port_Number);

	bcopy((char *)host -> h_addr, (char *)&Server_Address.sin_addr.s_addr, host -> h_length);

	// Connect to Remote server

	if(connect(remoteSocket, (struct sockaddr*)&Server_Address, (socklen_t)sizeof(Server_Address)) < 0 )
	{
		fprintf(stderr, "Error in connecting !\n"); 
		return -1;
	}
	// free(Host_Address);
	return remoteSocket;
}

int handle_request(int clientSocket, struct ParsedRequest *request, char *tempReq)
{
	char *Buffer = (char*)malloc(sizeof(char)*MAX_BYTES);
	strcpy(Buffer, "GET ");
	strcat(Buffer, request -> path);
	strcat(Buffer, " ");
	strcat(Buffer, request -> version);
	strcat(Buffer, "\r\n");

	size_t Length = strlen(Buffer);

	if (ParsedHeader_set(request, "Connection", "close") < 0){
		printf("Set header key not work\n");
	}

	if(ParsedHeader_get(request, "Host") == NULL)
	{
		if(ParsedHeader_set(request, "Host", request -> host) < 0){
			printf("Set \"Host\" Header key not working\n");
		}
	}

	if (ParsedRequest_unparse_headers(request, Buffer + Length, (size_t)MAX_BYTES - Length) < 0) {
		printf("Unparse failed\n");				
        // If this happens Still try to send request without header
	}

	int server_port = 80;				// Default Remote Server Port
	if(request -> port != NULL) server_port = atoi(request -> port);

	int remoteSocketID = connectRemoteServer(request -> host , server_port);

	if(remoteSocketID < 0) return -1;

	int bytes_send = send(remoteSocketID, Buffer, strlen(Buffer), 0);

	bzero(Buffer, MAX_BYTES);

	bytes_send = recv(remoteSocketID, Buffer, MAX_BYTES - 1, 0);

	char *temp_buffer = (char*)malloc(sizeof(char)*MAX_BYTES);

	int temp_buffer_size = MAX_BYTES;

	int temp_buffer_index = 0;

	while(bytes_send > 0){
		bytes_send = send(clientSocket, Buffer, bytes_send, 0);
		
		for(int i = 0; i < (bytes_send / sizeof(char)); i++){
			temp_buffer[temp_buffer_index] = Buffer[i];
			temp_buffer_index++;
		}

		temp_buffer_size += MAX_BYTES;
		temp_buffer = (char*)realloc(temp_buffer , temp_buffer_size);

		if(bytes_send < 0){
			perror("Error in sending data to client socket.\n");
			break;
		}

		bzero(Buffer, MAX_BYTES);

		bytes_send = recv(remoteSocketID, Buffer, MAX_BYTES - 1, 0);
	} 

	temp_buffer[temp_buffer_index]= '\0';

	free(Buffer);

	Add_Cache_Element(temp_buffer, strlen(temp_buffer), tempReq);
	printf("Done\n");

	free(temp_buffer);
	
 	close(remoteSocketID);

	return 0;
}

int checkHTTPversion(char *msg){
	int version = -1;

	if(strncmp(msg, "HTTP/1.1", 8) == 0){
		version = 1;
	}
	else if(strncmp(msg, "HTTP/1.0", 8) == 0){
		version = 1;										
	}
	else version = -1;

	return version;
}


void* Thread_Function(void *New_Socket){
    sem_wait(&Semaphore); // Using semaphores to create threads for each socket created for the Client Reuqest.

    int x;
    sem_getvalue(&Semaphore , &x);
    printf("Value of Semaphore is %d\n", x);

    int *temp_ = (int*)(New_Socket);

    int Socket = *temp_;

    int Bytes_send_by_client , Length;

    char *Buffer = (char*)calloc(MAX_BYTES , sizeof(char)); // Creating Buffer (Arrays) of 4kb for a Client
    bzero(Buffer , MAX_BYTES);

    // Recieveing Requests from the client
    Bytes_send_by_client = recv(Socket, Buffer, MAX_BYTES, 0);

    while(Bytes_send_by_client > 0){
        // i.e Requests are coming by the clients.
        Length = strlen(Buffer);

        // Any http request ends by "\r\n\r\n"
        if(strstr(Buffer , "\r\n\r\n") == NULL){
            Bytes_send_by_client = recv(Socket, Buffer + Length, MAX_BYTES - Length, 0);
        }
        else break;
    }

    char *tempReq = (char*)malloc(strlen(Buffer)*sizeof(char) + 1);

    for(int i = 0; i < strlen(Buffer); i++) tempReq[i] = Buffer[i];

    // Checking for the Request in Cache
    Cache_element *temp = Find(tempReq);

    if(temp != NULL){
        int size = (temp -> Length) / sizeof(char);
        int Position = 0;
        char Response[MAX_BYTES];

        while(Position < size){
            bzero(Response , MAX_BYTES);

            for(int i = 0; i < MAX_BYTES; i++){
                Response[i] = temp->Data[Position];
                Position++;
            }
            send(Socket, Response, MAX_BYTES, 0);
        }

        printf("Data Retrived from the Cache.\n");
		printf("%s\n\n", Response);
    }
    else if(Bytes_send_by_client > 0){
		Length = strlen(Buffer); 
		//Parsing the request
		struct ParsedRequest* request = ParsedRequest_create();
		
        //ParsedRequest_parse returns 0 on success and -1 on failure.On success it stores parsed request in
        // the request
		if(ParsedRequest_parse(request, Buffer, Length) < 0) {
		   	printf("Parsing failed\n");
		}
		else{	
			bzero(Buffer, MAX_BYTES);

			if(!strcmp(request -> method , "GET")){  
				if(request -> host && request -> path && (checkHTTPversion(request -> version) == 1) )
				{
					Bytes_send_by_client = handle_request(Socket, request, tempReq);		// Handle GET request
					if(Bytes_send_by_client == -1){	
						sendErrorMessage(Socket, 500);
					}
				}
				else sendErrorMessage(Socket, 500);			// 500 Internal Error
			}
            else{
                printf("This code doesn't support any method other than GET\n");
            }
    
		}
        // Freeing up the request pointer
		ParsedRequest_destroy(request);
    }
    else if(Bytes_send_by_client < 0){
		perror("Error in receiving from Client.\n");
	}
	else if(Bytes_send_by_client == 0){
		printf("Client disconnected!\n");
	}

	shutdown(Socket, SHUT_RDWR);
	close(Socket);
	free(Buffer);
	sem_post(&Semaphore);	
	
	sem_getvalue(&Semaphore , &x);
	printf("Semaphore post value :%d\n", x);

	free(tempReq);

	return NULL;
}



int main(int argc , char * argv[]){
    // Client_Length used to store its address length.
    int Client_Socket_ID, Client_Length;

    struct sockaddr_in Server_Address , Client_Address;

    // Initializing the Semaphore
    sem_init(&Semaphore, 0, MAX_CLIENTS);

    // Initializing the Lock
    pthread_mutex_init(&Lock , NULL);

    if(argc == 2){
        Port_Number = atoi(argv[1]);
    }
    else{
        printf("Too few arguments\n");
        exit(1);
    }

    printf("Starting Proxy Server at Port Number : %d\n", Port_Number);

    // We made a socket for our proxy server i.e Proxy_Socket_ID;

    /*
        Initially every Client Request will interact with one Sockect having ID as Proxy_Socket_ID and
        then we will further make different sockets for each request.
    */

    Proxy_Socket_ID = socket(AF_INET, SOCK_STREAM, 0); // int = socket(int Domain, int Type, int Protocol)

    if(Proxy_Socket_ID < 0){
        perror("Failed to create a Socket\n");
        exit(1);
    }

    // Above socket is the global socket which we will reuse again and again.

    int Reuse = 1;

    if(setsockopt(Proxy_Socket_ID, SOL_SOCKET, SO_REUSEADDR, (const char*)&Reuse, sizeof(Reuse)) < 0){
        perror("setSockOpt Failed\n");
    }

    bzero((char*)(&Server_Address) , sizeof(Server_Address));
    Server_Address.sin_family = AF_INET; // Assigining the server IPV4
    Server_Address.sin_port = htons(Port_Number);
    Server_Address.sin_addr.s_addr = INADDR_ANY; // Assign the address of the server to it

    // Now we will perform binding
    // Binding makes sure that, the server will only listen to a particular IP address

    if(bind(Proxy_Socket_ID, (struct sockaddr*)&Server_Address, sizeof(Server_Address)) < 0){
        perror("Port is not available\n");
        exit(1);
    }

    printf("Binding on Port : %d\n", Port_Number);

    int Listen_Status = listen(Proxy_Socket_ID , MAX_CLIENTS);

    if(Listen_Status < 0){
        perror("Error in Listening\n");
        exit(1);
    }

    int i = 0;

    int Connected_Socket_ID[MAX_CLIENTS];

    // Creating sockets for the Multiple Client Requests
    while(1){
        bzero((char *)(&Client_Address) , sizeof(Client_Address));
        Client_Length = sizeof(Client_Address);

        // Accepting the connections
        Client_Socket_ID = accept(Proxy_Socket_ID, (struct sockaddr*)&Client_Address, (socklen_t*)&Client_Length);

        if(Client_Socket_ID < 0){
            fprintf(stderr, "Error in Accepting Connections\n");
            exit(1);
        }
        else Connected_Socket_ID[i] = Client_Socket_ID; // Storing the accepted Clients to our Proxy server in an array.

        // Getting the IP Address and Port Number of the client
        struct sockaddr_in* Client_Pointer = (struct sockaddr_in *)&Client_Address;
        struct in_addr IP_Address = Client_Pointer -> sin_addr;

        char String[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN: Default IP Address size

        inet_ntop(AF_INET, &IP_Address, String, INET_ADDRSTRLEN);
        printf("\nClient is connected with Port Number %d and the IP address is %s\n", ntohs(Client_Address.sin_port), String);

        // Creating a thread for each socket
        pthread_create(&Thread_ID[i], NULL, Thread_Function, (void*)&Connected_Socket_ID[i]);

        i++;
    }

    close(Proxy_Socket_ID);
    return 0;
}

Cache_element* Find(char* URL){
	Cache_element *Site = NULL;

	int Lock_Value = pthread_mutex_lock(&Lock);

	printf("Remove Cache Lock Acquired : %d\n", Lock_Value);

	if(Head != NULL){
		Site = Head;

		while(Site != NULL){
			if(!strcmp(Site -> URL , URL)){
				printf("LRU Time Track Before : %ld\n", Site -> LRU_time_track);
				printf("URL Found\n");

				// Updating the time.
				Site -> LRU_time_track = time(NULL);
				printf("LRU Time Track After : %ld\n", Site -> LRU_time_track);
				break;
			}
			Site = Site -> Next;
		}
	}
	else printf("URL Not Found\n");

	Lock_Value = pthread_mutex_unlock(&Lock);

	printf("Remove Cache Lock Unlocked : %d\n", Lock_Value);

	return Site;
}

void Remove_Cache_Element(){
	Cache_element *x, *y, *temp;

	int Lock_Value = pthread_mutex_lock(&Lock);

	printf("Remove Cache Lock Acquired : %d\n", Lock_Value);

	if(Head != NULL){
		for(x = Head, y = Head, temp = Head; (y -> Next) != NULL; y = y -> Next){
			if(((y -> Next) -> LRU_time_track) < (temp -> LRU_time_track)){
				temp = y -> Next;
				x = y;
			}
		}
		if(temp == Head) Head = Head -> Next;
		else x -> Next = temp -> Next;

		Cache_Size = Cache_Size - ((temp -> Length) + sizeof(Cache_element) + strlen(temp -> URL) + 1);

		free(temp -> Data);     		
		free(temp -> URL);
		free(temp);
	}

	Lock_Value = pthread_mutex_unlock(&Lock);

	printf("Remove Cache Lock Unlocked : %d\n", Lock_Value);
}

int Add_Cache_Element(char *Data, int size, char *URL){
	int Lock_Value = pthread_mutex_lock(&Lock);

	printf("Remove Cache Lock Acquired : %d\n", Lock_Value);

	int Element_Size = size + 1 + strlen(URL) + sizeof(Cache_element);

	if(Element_Size > MAX_ELEMENT_SIZE){
		Lock_Value = pthread_mutex_unlock(&Lock);

		printf("Remove Cache Lock Unlocked : %d\n", Lock_Value);

		return 0;
	}
	else{
		while(Cache_Size + Element_Size > MAX_SIZE) Remove_Cache_Element();

		Cache_element *Element = (Cache_element*)malloc(sizeof(Cache_element));

		Element -> Data = (char*)malloc(size + 1);
		strcpy(Element -> Data , Data);

		Element -> URL = (char*)malloc(1 + (strlen(URL) * sizeof(char)));
		strcpy(Element -> URL , URL);

		Element -> LRU_time_track = time(NULL);

		Element -> Next = Head;
		Element -> Length = size;
		Head = Element;
		Cache_Size += Element_Size;

		Lock_Value = pthread_mutex_unlock(&Lock);

		printf("Remove Cache Lock Unlocked : %d\n", Lock_Value);

		return 1;
	}

	return 0;
}

