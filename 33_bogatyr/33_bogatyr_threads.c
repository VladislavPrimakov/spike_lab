//Богатыри-процессы(одноранговые, без дирижера) должны скоординироваться через сообщения, 
//чтобы пропеть текст песни(по умолчанию "Hello world!",но должен быть аргумент командной строки для указания другого файла).
//Каждый процесс выбирает уникальную букву из текста и поет только ее в нужные моменты, 
//выводя на stdout без лишней информации(при минимальном уровне отладки).
//Программа должна принимать аргументы : количество богатырей, файл с текстом песни и уровень детализации отладочного вывода.
//Все процессы исполняют идентичный алгоритм

#include <bits/time.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_NAME "/queue"
#define QUEUE_SIZE_NAME 64
char default_song[]="Hello World!\n";
char* song=default_song;
int log_level=1;
int num_workers=33;

typedef enum {
	IDLE,
	WANTED,
	READY,
	KILL,
	WAIT
} State;

typedef enum {
	CAPTURE,
	SING,
	READY_TO_SING,
	REPLY_OK,
	REPLY_FAIL
} MessageType;

typedef struct {
	MessageType type;
	unsigned long long timestamp;
	char letter;
	int index_letter;
	int id_sender;
	int id_receiver;
} Message;


typedef struct {
	int id;
	char letter;
	unsigned long long timestamp;
	State state;
	int replies_needed;
	int replies_received;
	int replies_received_ok;
	mqd_t my_mq;
	mqd_t* other_mqs; // [num_workers]
	int index_letter;
	Message stack_received_msg[1000];
	int index_stack_received_msg;
	int replies_ready_to_sing;
	char* uniq; // [num_workers + 1]
	pthread_mutex_t mutex;
	pthread_cond_t cond;
} ChildState;

void child_send_capture(ChildState* child_state);

void child_reply_capture(ChildState* child_state, const Message* received_msg);

void child_reply_reply(ChildState* child_state, const Message* received_msg);

void child_send_ready_to_sing(ChildState* child_state);

void child_reply_ready_to_sing(ChildState* child_state, const Message* received_msg);

void child_send_sing(ChildState* child_state, int index_letter);

void child_reply_sing(ChildState* child_state, const Message* received_msg);

void child_mq_send(ChildState* child_state, const Message* msg);

unsigned long long get_timestamp_ns() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long) ts.tv_sec*1000000000+ts.tv_nsec;
}

void dlog(int level, const char* format, ...) {
	va_list args;
	if (log_level<level)
		return;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

void child_init(ChildState* child_state, int id) {
	child_state->id=id;
	child_state->letter='\0';
	child_state->timestamp=ULLONG_MAX;
	child_state->state=IDLE;
	child_state->replies_needed=num_workers-1;
	child_state->replies_received=0;
	child_state->replies_received_ok=0;
	child_state->index_letter=0;
	child_state->index_stack_received_msg=0;
	child_state->replies_ready_to_sing=0;
	child_state->other_mqs=malloc(sizeof(mqd_t)*num_workers);
	child_state->uniq=calloc((num_workers+1), sizeof(char));
	pthread_mutex_init(&child_state->mutex, NULL);
	pthread_cond_init(&child_state->cond, NULL);
	char my_queue_name[QUEUE_SIZE_NAME];
	snprintf(my_queue_name, sizeof(my_queue_name), "%s%d", QUEUE_NAME, id);
	child_state->my_mq=mq_open(my_queue_name, O_RDONLY);
	if (child_state->my_mq<0) {
		perror("mq_open");
		exit(1);
	}
	for (int i=0; i<num_workers; i++) {
		char other_queue_name[QUEUE_SIZE_NAME];
		snprintf(other_queue_name, sizeof(other_queue_name), "%s%d", QUEUE_NAME, i);
		child_state->other_mqs[i]=mq_open(other_queue_name, O_WRONLY);
		if (child_state->other_mqs[i]<0) {
			perror("mq_open");
			exit(1);
		}
	}
}

void child_close(ChildState* child_state) {
	mq_close(child_state->my_mq);
	for (int i=0; i<num_workers; i++) {
		mq_close(child_state->other_mqs[i]);
	}
	free(child_state->other_mqs);
	free(child_state->uniq);
	free(child_state);
}

void child_send_capture(ChildState* child_state) {
	if (child_state->state==IDLE) {
		if (strchr(child_state->uniq, song[child_state->index_letter])) {
			pthread_mutex_lock(&child_state->mutex);
			child_state->index_letter++;
			pthread_mutex_unlock(&child_state->mutex);
		} else {
			dlog(2, "Worker %d tries to capture letter '%c'\n", child_state->id, song[child_state->index_letter]);
			child_state->letter=song[child_state->index_letter];
			child_state->replies_received=0;
			child_state->replies_received_ok=0;
			child_state->state=WANTED;
			child_state->timestamp=get_timestamp_ns();
			for (int i=0; i<num_workers; i++) {
				if (i==child_state->id) {
					continue;
				}
				Message msg={.type=CAPTURE, .letter=child_state->letter, .id_sender=child_state->id, .id_receiver=i, .timestamp=child_state->timestamp};
				dlog(2, "Worker %d send {CAPTURE} {%c} to Worker %d\n", child_state->id, child_state->letter, i);
				child_mq_send(child_state, &msg);
			}
		}
	}
}

void child_reply_capture(ChildState* child_state, const Message* received_msg) {
	Message msg={.type=REPLY_OK, .id_sender=child_state->id, .id_receiver=received_msg->id_sender};
	if (child_state->state==WANTED) {
		if (child_state->letter==
			received_msg->letter&&
			child_state->timestamp<
			received_msg->timestamp) {
			msg.type=REPLY_FAIL;
		}
	}
	if (child_state->state==READY) {
		if (child_state->letter==received_msg->letter) {
			msg.type=REPLY_FAIL;
		}
	}
	dlog(2, "Worker %d send {REPLY} {%c} to Worker %d\n", child_state->id, child_state->letter, msg.id_receiver);
	child_mq_send(child_state, &msg);
}

void child_reply_reply(ChildState* child_state, const Message* received_msg) {
	if (received_msg->type==REPLY_OK) {
		child_state->replies_received_ok++;
	}
	if (received_msg->type==REPLY_FAIL) {
		if (strchr(child_state->uniq, received_msg->letter)==NULL) {
			char tmp[2]={received_msg->letter};
			strcat(child_state->uniq, tmp);
		}
	}
	child_state->replies_received++;
	dlog(2, "Worker %d received reply {%s} from worker %d. Total replies for capture {%c} - [%d/%d|%d] \n",
		 child_state->id, received_msg->type==REPLY_OK ? "OK" : "FAIL", received_msg->id_sender, child_state->letter, child_state->replies_received_ok,
		 child_state->replies_received-child_state->replies_received_ok, child_state->replies_needed);
	if (child_state->replies_received_ok==child_state->replies_needed) {
		dlog(1, "Worker %d {CAPTURED|READY_TO_SING} letter %c\n", child_state->id, child_state->letter);

		child_send_ready_to_sing(child_state);
	} else if (child_state->replies_received==child_state->replies_needed) {
		child_state->state=IDLE;
		child_state->index_letter++;
		child_state->letter='\0';
		if (child_state->index_letter>=strlen(song)) {
			dlog(1, "Worker %d {READY_TO_SING}\n", child_state->id);
			child_send_ready_to_sing(child_state);
		}
	}
}

void child_send_ready_to_sing(ChildState* child_state) {
	child_state->state=READY;
	for (int i=0; i<num_workers; i++) {
		Message msg={.type=READY_TO_SING, .id_sender=child_state->id, .id_receiver=i};
		child_mq_send(child_state, &msg);
	}
}

void child_reply_ready_to_sing(ChildState* child_state, const Message* received_msg) {
	child_state->replies_ready_to_sing++;
	if (child_state->letter==song[0]) {
		// check if everyone ready to sing and my letter first
		bool is_everyone_ready_to_sing=true;
		if (child_state->replies_ready_to_sing<num_workers) {
			is_everyone_ready_to_sing=false;
		}
		if (is_everyone_ready_to_sing) {
			dlog(1, "Everyone is ready to sing.\n");
			child_send_sing(child_state, 0);
		}
	}
}

void child_send_sing(ChildState* child_state, int index_letter) {
	for (int i=0; i<num_workers; i++) {
		Message msg={.type=SING, .index_letter=index_letter, .id_sender=child_state->id, .id_receiver=i};
		child_mq_send(child_state, &msg);
	}
}

void child_reply_sing(ChildState* child_state, const Message* received_msg) {
	if (received_msg->index_letter>=strlen(song)) {
		child_state->state=KILL;
	} else if (child_state->letter==song[received_msg->index_letter]) {
		if (log_level>0) {
			printf("Worker %d is singing letter {%c}\n", child_state->id, child_state->letter);
		} else {
			write(1, &child_state->letter, 1);
		}
		child_send_sing(child_state, received_msg->index_letter+1);
	}
}

void child_mq_send(ChildState* child_state, const Message* msg) {
	if (mq_send(child_state->other_mqs[msg->id_receiver], (const char*) msg, sizeof(Message), 0)<0) {
		perror("mq_send()");
		exit(1);
	}
}

void* child_thread_receiver(void* arg) {
	ChildState* child_state=(ChildState*) arg;
	while (child_state->state!=KILL) {
		Message received_msg;
		ssize_t bytes_read=mq_receive(child_state->my_mq, (char*) &received_msg, sizeof(Message), NULL);
		if (bytes_read>0) {
			pthread_mutex_lock(&child_state->mutex);
			child_state->stack_received_msg[child_state->index_stack_received_msg++]=received_msg;
			pthread_cond_signal(&child_state->cond);
			pthread_mutex_unlock(&child_state->mutex);
		} else {
			perror("mq_receive()");
			exit(1);
		}
	}
	return NULL;
}

void child(const int id) {
	ChildState* child_state=(ChildState*) malloc(sizeof(ChildState));
	child_init(child_state, id);
	pthread_t thread_receiver;
	pthread_create(&thread_receiver, NULL, child_thread_receiver, child_state);

	while (child_state->state!=KILL) {
		// capture letter
		child_send_capture(child_state);
		// get one message
		pthread_mutex_lock(&child_state->mutex);
		while (child_state->index_stack_received_msg==0) {
			pthread_cond_wait(&child_state->cond, &child_state->mutex);
		}
		const Message received_msg=child_state->stack_received_msg[0];
		child_state->index_stack_received_msg--;
		memmove(child_state->stack_received_msg, child_state->stack_received_msg+1, sizeof(Message)*child_state->index_stack_received_msg);
		pthread_mutex_unlock(&child_state->mutex);

		switch (received_msg.type) {
		case CAPTURE:
		child_reply_capture(child_state, &received_msg);
		break;
		case REPLY_OK:
		case REPLY_FAIL:
		child_reply_reply(child_state, &received_msg);
		break;
		case READY_TO_SING:
		child_reply_ready_to_sing(child_state, &received_msg);
		break;
		case SING:
		child_reply_sing(child_state, &received_msg);
		break;
		}
	}
	pthread_cancel(thread_receiver);
	pthread_join(thread_receiver, NULL);
	child_close(child_state);
}

void unlink_mq() {
	for (int i=0; i<num_workers; ++i) {
		char final_queue_name[QUEUE_SIZE_NAME];
		snprintf(final_queue_name, sizeof(final_queue_name), "%s%d", QUEUE_NAME, i);
		mq_unlink(final_queue_name);
	}
}


int main(int argc, char* argv[]) {
	while (1) {
		int option_index=0;
		static struct option long_options[]={
			{"log-level", required_argument, 0, 'l'},
			{"workers", required_argument, 0, 'w'},
			{"input-file", required_argument, 0, 'f'},
			{"help", no_argument, 0, 'h'},
			{0, 0, 0, 0}
		};
		int c=getopt_long(argc, argv, "l:w:f:h", long_options, &option_index);
		if (c==-1) {
			break;
		}
		switch (c) {
		case 'l':
		log_level=atoi(optarg);
		break;
		case 'w':
		num_workers=atoi(optarg);
		break;
		case 'f':
		{
			int fd=open(optarg, O_RDONLY);
			if (fd<0) {
				perror(optarg);
				return -1;
			}
			struct stat st;
			if (fstat(fd, &st)<0) {
				perror("fstat");
				close(fd);
				return -1;
			}
			song=malloc(st.st_size+1);
			if (!song) {
				perror("malloc");
				close(fd);
				return -1;
			}
			ssize_t bytes_read=read(fd, song, st.st_size);
			if (bytes_read<0) {
				perror("read");
				free(song);
				close(fd);
				return -1;
			}
			song[bytes_read]='\0';
			close(fd);
			break;
		}

		case 'h':
		fprintf(stderr, "Usage: %s [options]\n", argv[0]);
		fprintf(stderr, "\t-l, --log-level,\t\tverbose level [0-3]\n");
		fprintf(stderr, "\t-w, --workers,\t\tnum workers\n");
		fprintf(stderr, "\t-f, --input-file,\t\tinput file\n");
		fprintf(stderr, "\t-h, --help,\t\tdisplay with help\n");
		exit(0);

		case '?':
		fprintf(stderr, "Invalid option\n");
		exit(1);
		}
	}
	unlink_mq();
	for (int i=0; i<num_workers; ++i) {
		struct mq_attr attr={
			.mq_flags=0,
			.mq_maxmsg=10,
			.mq_msgsize=sizeof(Message)
		};
		char final_queue_name[QUEUE_SIZE_NAME];
		snprintf(final_queue_name, sizeof(final_queue_name), "%s%d", QUEUE_NAME, i);
		const mqd_t mq=mq_open(final_queue_name, O_CREAT|O_RDWR|O_EXCL, 0644, &attr);
		if (mq==(mqd_t) -1) {
			perror("mq_open");
			exit(1);
		}
		mq_close(mq);
	}

	for (int i=0; i<num_workers; ++i) {
		pid_t pid=fork();
		if (pid==-1) {
			perror("fork");
			exit(1);
		}
		if (pid==0) {
			child(i);
			exit(0);
		}
	}

	for (int i=0; i<num_workers; ++i) {
		wait(NULL);
	}

	unlink_mq();
	if (song!=default_song) {
		free(song);
	}
	return 0;
}
