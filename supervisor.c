#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>

#define UNIX_PATH_MAX 108
//dimensione della tabella, ossio numero massimo di client da stimare
#define MAX 50

//array di pipe usato per la comunicazione tra servers e supervisor
int** pfd;

//struct per i messaggi mandati dai server al supervisor
typedef struct _mex {
	uint64_t id;
	int estim;
} mex;

//struct per le stime effettuate dal supervisor
typedef struct _est {
	uint64_t id;
	int estim;
	int n;
} est;


/*
Funzione che, se necessario, porta l'id (a 64 bit, per il quale non esiste
una funzione standard POSIX) da network byte order a host byte order. Per la
realizzazione vengono utilizzati due elementi di appoggio a 32 bit, contenenti
le due metà dell'id, sulle quali viene chiamata la funzione htonl.
Successivamente le due parti vengono scambiate tra loro e riunite.
*/

uint64_t ntohll(uint64_t id) {
	int num = 42;
	if (*(char *)&num == 42) {
	//test per verificare che l'architettura usi little endian
		uint32_t high_part = htonl((uint32_t)(id >> 32));
		//htonl viene chiamata sulle due metà dell'id, ciascuna a 32 bit
		uint32_t low_part = htonl((uint32_t)(id & 0xFFFFFFFFLL));
		return ( (( (uint64_t)low_part ) << 32) | high_part);
	}
	else {
		return id;
	}
}


/*
Funzione che calcola la minima differenza in millisecondi tra i messaggi ricevuti
dai worker, memorizzati nell'array a di dimensione n (passati come parametri).
*/

long int sec_est (unsigned long long int* a, int n) {
	if (a[0]==0 || a[1]==0) {
	//il worker ha ricevuto 0 o 1 messaggi, e non può stimare il secret
		return 0;
	}
	else {
		int mean = 4000000000;
		int j = 0, i;
		
		for (i=1;i<n;i++) {
			if (a[i]-a[j] < mean) {
				mean = a[i]-a[j];
			}
			j++;
		}
		
		return mean;
	}
}


/*
Thread worker, che riceve come argomento un array con la socket cs già connessa
ad UN determinato client e l'identificatore i del server da cui è stato lanciato.
Il suo compito è di attendere messaggi da parte del client, per poi stimare
meglio che può il suo secret e passarlo tramite pipe anonima al supervisor
insieme all'id del client con cui ha comunicato. Il supervisor saprà da quale
server proviene il messaggio perchè lo troverà in posizione i dell'array di pipe.
*/

static void* worker (void*arg) {
	uint64_t n_id=0, id=0;
	int j=0, n=0;
	int estim;
	int* temp = (int*) arg;
	unsigned long long int tm[MAX];
	int cs = temp[0];
	int i = temp[1];
	struct timeval t;
	
	while ((n = read(cs, &n_id, sizeof(uint64_t))) == sizeof(uint64_t)) {
		gettimeofday(&t,NULL);
		if (j==0)
		//nel primo ciclo memorizza l'id convertito in hbo in id
			id = ntohll(n_id);
		tm[j] = (((unsigned long long int)t.tv_sec)*1000+((unsigned long long int)t.tv_usec/1000));
		//memorizzazione in tm degli orari (con precisione al millisecondo) di ricezione dei messaggi
		printf("SERVER %d INCOMING FROM %016llX @ %lld\n", (i+1), id, tm[j]);
		j++;
	}
	
	close(cs);
	estim = (int) sec_est(tm,j);
	
	//invio della stima, ma solo nel caso sia consistente
	if (id!=0 && estim>0) {
		printf("SERVER %d CLOSING %016llX ESTIMATE %d\n", (i+1), id, estim);
		mex m = {id,estim};
		write(pfd[i][1], &m, sizeof(mex));
	}
	
	pthread_exit(NULL);
}


/*
Thread dispatcher, che accetta le connessioni dei client sulla socket di indirizzo
OOB-SERVER-i (dove i è l'identificatore del server da cui è stato lanciato, passato
nell'array di parametri insieme alla socket già creata, binded e in listening) e
crea per ognuna di esse un worker a cui affida la comunicazione con ciascun client.
Cicla all'infinito, fino a ricevere un SIGTERM dal supervisor al momento del doppio SIGINT.
*/

static void* dispatcher (void*arg) {
	int i,ss;
	pthread_t t;
	int* temp = (int*) arg;
	ss = temp[0];
	i = temp[1];
	
	while(1) {
		int cs;
		
		if ((cs = accept(ss,NULL,NULL)) == -1) {
			perror("dispatcher: accept error");
		}
		
		if (cs > 0) {
			printf("SERVER %d CONNECT FROM CLIENT\n", (i+1));
			static int arr[2];
			arr[0] = cs;
			arr[1] = i;
			pthread_create(&t,NULL,&worker,arr);
		}
	}
	
	pthread_exit(NULL);
}


/*
Procedura chiamata da ogni processo server al momento della creazione. Apre una
socket nel dominio AF_UNIX, chiama la bind con parametro sa (il cui sun_path è
OOB-SERVER-i) e la pone in ascolto. Maschera SIGINT per evitare la terminazione
nel caso di invio del segnale di interruzione da tastiera tramite Ctrl-C e delega
l'accettazione di connessioni sulla socket al thread dispatcher (UNO per ogni server)
e la comunicazione e la stima del secret, indirettamente, ai worker (TANTI per ogni
server, uno per ogni client che si pone in comunicazione con esso).
*/

void server(int i, int* servers) {
        
        servers[i] = fork();
        
        if (servers[i] == 0) {
                printf("SERVER %d ACTIVE\n", (i+1));
                struct sockaddr_un sa;
                int ss;
                pthread_t t;
                //preparazione dell'indirizzo della socket
                char s[15] = "OOB-server-";
                sprintf((s+11), "%d\0", (i+1));
        	strncpy(sa.sun_path,s,UNIX_PATH_MAX);
                sa.sun_family=AF_UNIX;
                
                if ((ss = socket(AF_UNIX,SOCK_STREAM,0)) == -1) {
                        perror("server: error creating socket");
                }
                
                if (bind(ss,(struct sockaddr *)&sa,sizeof(sa)) == -1) {
                        perror("server: error binding socket");
                }
                
                if (listen(ss,SOMAXCONN) == -1) {
                        perror("server: error listening");
                }
                
                static int arr[2];
                arr[0] = ss;
                arr[1] = i;
                
                sigset_t set;
                sigfillset(&set);
                pthread_sigmask(SIG_SETMASK,&set,NULL);
                
                pthread_create(&t,NULL,&dispatcher,arr);
		pthread_join(t,NULL);
		exit(0);
        }
}

//array contenente i pid dei servers
int* servers;

//numero di server da lanciare
int k;

//"tabella" da stampare alla ricezione di
//singolo (->stderr) o doppio (->stdout) SIGINT
est* tab;

//dimensione della tabella
int n;

//contatore per riconoscere il doppio SIGINT
volatile sig_atomic_t sig_counter;


/*
Procedura addetta alla stampa su fd (che sarà stderr o stdout)
della tabella delle stime effettuate dal supervisor.
*/

void print (int fd) {
	int i;
	char s[57];
	for (i=0;i<n;i++) {
		sprintf(s, "SUPERVISOR ESTIMATE %d FOR %016llX BASED ON %d\n", tab[i].estim, tab[i].id, tab[i].n);
		int l = strlen(s);
		write(fd,&s,l);
	}
}


/*
Gestore del segnale SIGALRM, che si occupa del reset del
contatore per i SIGINT se passa un secondo tra uno e l'altro
*/

static void sigalrm_handler (int signum) {
	sig_counter=0;
}


/*
Gestore del segnale SIGINT, che se sig_counter=0 stampa su stderr
la tabella delle stime e pone un allarme dopo un secondo per
resettare il contatore che incrementa, altrimenti stampa la stessa
tabella su stdout, manda un segnale SIGTERM a ogni server e fa
terminare anche l'esecuzione del supervisor.
*/

static void sigint_handler (int signum) {
	if (!sig_counter) {
		sig_counter = 1;
		alarm(1);
		
		print(2);
	}
	else {
		write(1,"\n",1);
		
		print(1);
		
		write(1, "SUPERVISOR EXITING\n", 19);
		int i;
		
		for(i=0;i<k;i++)
			kill(servers[i],9);
		
		_exit(EXIT_SUCCESS);
	}
}


int gcd(int a, int b) {
    int temp;
    while (b != 0) {
        temp = a % b;
        a = b;
        b = temp;
    }
    return a;
}


int min(int a, int b) {
	if (a<b)
		return a;
	else
		return b;
}


/*
Procedura che aggiorna la tabella delle stime dei secret, nel caso
in cui ne venga calcolata una migliore di quella precedente o nel
caso in cui non sia stata ancora ricevuta alcuna stima per il
secret di quel client.
*/

void insert (mex m) {
	
	int i = 0;
	int estim = m.estim;
	uint64_t id = m.id;
	int found = 0;
	
	//n var globale, dim di tab
	while (i<n && !found) {
		if (id == tab[i].id) {
		
			int temp = gcd(estim, tab[i].estim);
			
			//prendo 21 come limite probabile di un qualsiasi divisore di stime
			//imprecise, che potrebbero essere anche prime tra loro
			if (temp>21)
				tab[i].estim = temp;
			else
				tab[i].estim = min(estim,tab[i].estim);
			
			(tab[i].n)++;
			found = 1;
			
		}
		
		else i++;
		
	}
	
	if (!found) {
		
		tab[n].id = id;
		tab[n].estim = estim;		
		
		tab[n].n = 1;
		n++;
		
	}
}


/*
SUPERVISOR. Genera i server tramite le fork nella procedura "server", e
mantiene con essi la possibilità di comunicare grazie all'array di pipe
pfd. Le stime dei secret ricevute dai server vengono memorizzate, tramite
la chiamata alla procedura insert, in tab, e stampate solo al momento della
ricezione di singolo/doppio SIGINT.
*/

int main (int argc, char *argv[]) {

	if (argc != 2) {
		printf("Usage: supervisor (int) k -> launch supervisor and k servers\n");
		return -1;
	}

	int i;
	sscanf(argv[1],"%d",&k);
	printf("SUPERVISOR STARTING %d\n", k);
	pfd = (int **) malloc(k*sizeof(int*));
	
	//apertura delle pipe
	for(i=0;i<k;i++) {
		pfd[i] = (int *) malloc(2*sizeof(int));
		pipe(pfd[i]);
	}

	servers = (int *) malloc(k*sizeof(int));

	//fork dei servers
	for(i=0;i<k;i++) {
		server(i, servers);
	}

	//setting di O_NONBLOCK per le read sulle pipe
	for (i=0;i<k;i++) {
		int flags = fcntl(pfd[i][0], F_GETFL, 0);
		fcntl(pfd[i][0], F_SETFL, flags | O_NONBLOCK);
	}
	
	//definizioni dei gestori dei segnali
	struct sigaction sint, salrm;
	bzero(&sint, sizeof(sint));
	bzero(&salrm, sizeof(salrm));
	sint.sa_handler = sigint_handler;
	sint.sa_flags = SA_RESTART;
	salrm.sa_handler = sigalrm_handler;
	salrm.sa_flags = SA_RESTART;
	sigaction(SIGINT,&sint,NULL);
	sigaction(SIGALRM,&salrm,NULL);
	
	tab = malloc(MAX*sizeof(est));
	n=0; //var glob, dim di tab
	sig_counter=0;
	mex m;
	
	//comunicazione con i server in un ciclo infinito
	while (1) {
		for (i=0;i<k;i++) {
			if(read(pfd[i][0],&m,sizeof(mex)) == sizeof(mex)) {
				printf("SUPERVISOR ESTIMATE %d FOR %016llX FROM %d\n", m.estim,m.id,(i+1));
				insert(m);
			}
		}
	}
	
	return 0;
}
