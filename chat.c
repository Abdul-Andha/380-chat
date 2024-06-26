#include <gtk/gtk.h>
#include <glib/gunicode.h> /* for utf8 strlen */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <getopt.h>
#include "dh.h"
#include "keys.h"
#include "util.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

// not available by default on all systems
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

// Define message types
#define MSG_TYPE_KEY 0x01
#define MSG_TYPE_TEXT 0x02

static GtkTextBuffer *tbuf; /* transcript buffer */
static GtkTextBuffer *mbuf; /* message buffer */
static GtkTextView *tview;	/* view for transcript */
static GtkTextMark *mark;	/* used for scrolling to end of transcript, etc */

static pthread_t trecv; /* wait for incoming messagess and post to queue */
void *recvMsg(void *);	/* for trecv */

#define max(a, b) \
	({ typeof(a) _a = a;    \
	 typeof(b) _b = b;    \
	 _a > _b ? _a : _b; })

/* network stuff... */

static int listensock, sockfd;
static int isclient = 1;

/* global variables for session keys*/
dhKey sessionKeys;
unsigned char sharedSecret[128];

static void error(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

// For debugging purposes, to be able to read keybuff from dhFinal
void print_hex(const unsigned char *buffer, size_t length) {
    printf("Key Buffer (Hex): ");
    for (size_t i = 0; i < length; ++i) {
        printf("%02x", buffer[i]);
    }
    printf("\n");
}

int initServerNet(int port)
{
	int reuse = 1;
	struct sockaddr_in serv_addr;
	listensock = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listensock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	/* NOTE: might not need the above if you make sure the client closes first */
	if (listensock < 0)
		error("ERROR opening socket");
	bzero((char *)&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);
	if (bind(listensock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
		error("ERROR on binding");
	fprintf(stderr, "listening on port %i...\n", port);
	listen(listensock, 1);
	socklen_t clilen;
	struct sockaddr_in cli_addr;
	sockfd = accept(listensock, (struct sockaddr *)&cli_addr, &clilen);
	if (sockfd < 0)
		error("error on accept");
	close(listensock);
	fprintf(stderr, "connection made, starting session...\n");
	/* at this point, should be able to send/recv on sockfd */
	
	/* Generate server's temp public and secret keys */

	initKey(&sessionKeys);
	dhGenk(&sessionKeys);

	if(serialize_mpz(sockfd, sessionKeys.PK) == 0)
    {
		error("Something went wrong sending public key (server) \n");
	} else {
		printf("sent public key (server) \n");
	}

	char* pk_str = mpz_get_str(NULL, 10, sessionKeys.PK);
    printf("Server public key (pk): %s\n", pk_str);
	free(pk_str);

	mpz_t clientPubKey;
	mpz_init(clientPubKey);
	if(deserialize_mpz(clientPubKey, sockfd) != 0)
    {
		error("Something went wrong recieving client public key \n");
	} else {
		printf("receieved client key \n");
	}

	dhKey friendsLongTerm;
	dhKey myLongTerm;

	if (readDH("clientKey.pub", &friendsLongTerm) == -1)
	{
		error("Failed to read client's public key");
		return 0; // or handle the error in an appropriate way
	}
	if (readDH("serverKey", &myLongTerm) == -1)
	{
		error("Failed to read my long term public key");
		return 0; // or handle the error in an appropriate way
	}

	dh3Final(myLongTerm.SK, myLongTerm.PK, sessionKeys.SK, sessionKeys.PK, friendsLongTerm.PK, clientPubKey, sharedSecret, 128);
	printf("Shared Secret: ");
	print_hex(sharedSecret, SHA256_DIGEST_LENGTH);

	return 0;
}

static int initClientNet(char *hostname, int port)
{
	struct sockaddr_in serv_addr;
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	struct hostent *server;
	if (sockfd < 0)
		error("ERROR opening socket");
	server = gethostbyname(hostname);
	if (server == NULL)
	{
		fprintf(stderr, "ERROR, no such host\n");
		exit(0);
	}
	bzero((char *)&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
	serv_addr.sin_port = htons(port);
	if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
		error("ERROR connecting");
	/* at this point, should be able to send/recv on sockfd */

	/* Generate client's temp public and secret keys */
	initKey(&sessionKeys);
	dhGenk(&sessionKeys);

	if(serialize_mpz(sockfd, sessionKeys.PK) == 0)
    {
		error("Something went wrong sending public key (client) \n");
	} else {
		printf("sent public key (client) \n");
	}


	char* pk_str = mpz_get_str(NULL, 10, sessionKeys.PK);
    printf("Client public key (pk): %s\n", pk_str);
	free(pk_str);

	mpz_t serverPubKey;
	mpz_init(serverPubKey);
	if(deserialize_mpz(serverPubKey, sockfd) != 0)
    {
		error("Something went wrong recieving server public key \n");
	} else {
		printf("receieved server key \n");
	}

	dhKey friendsLongTerm;
	dhKey myLongTerm;

	if (readDH("serverKey.pub", &friendsLongTerm) == -1)
	{
		error("Failed to read server's public key");
		return 0; // or handle the error in an appropriate way
	}
	if (readDH("clientKey", &myLongTerm) == -1)
	{
		error("Failed to read my long term public key");
		return 0; // or handle the error in an appropriate way
	}

	dh3Final(myLongTerm.SK, myLongTerm.PK, sessionKeys.SK, sessionKeys.PK, friendsLongTerm.PK, serverPubKey, sharedSecret, 128);
	printf("Shared Secret: ");
	print_hex(sharedSecret, SHA256_DIGEST_LENGTH);

	return 0;
}

// NOT SKELETON CODE
int generateLongTermKey(char *fname) {
	dhKey key;
	if (readDH(fname, &key) == -1) {
		initKey(&key);
		dhGenk(&key);
		char *filename = fname;
		if (writeDH(filename, &key) != 0) {
			printf("failed to write key to file %s\n", filename);
			return -1;
		} else {
			fprintf(stderr, "wrote key to file %s\n", filename);
			return 0;
		}
	}
	return 0;
}
//

static int shutdownNetwork()
{
	shutdown(sockfd, 2);
	unsigned char dummy[64];
	ssize_t r;
	do
	{
		r = recv(sockfd, dummy, 64, 0);
	} while (r != 0 && r != -1);
	close(sockfd);
	return 0;
}

/* end network stuff. */

static const char *usage =
	"Usage: %s [OPTIONS]...\n"
	"Secure chat (CCNY computer security project).\n\n"
	"   -c, --connect HOST  Attempt a connection to HOST.\n"
	"   -l, --listen        Listen for new connections.\n"
	"   -p, --port    PORT  Listen or connect on PORT (defaults to 1337).\n"
	"   -h, --help          show this message and exit.\n";

/* Append message to transcript with optional styling.  NOTE: tagnames, if not
 * NULL, must have it's last pointer be NULL to denote its end.  We also require
 * that messsage is a NULL terminated string.  If ensurenewline is non-zero, then
 * a newline may be added at the end of the string (possibly overwriting the \0
 * char!) and the view will be scrolled to ensure the added line is visible.  */
static void tsappend(char *message, char **tagnames, int ensurenewline)
{
	GtkTextIter t0;
	gtk_text_buffer_get_end_iter(tbuf, &t0);
	size_t len = g_utf8_strlen(message, -1);
	if (ensurenewline && message[len - 1] != '\n')
		message[len++] = '\n';
	gtk_text_buffer_insert(tbuf, &t0, message, len);
	GtkTextIter t1;
	gtk_text_buffer_get_end_iter(tbuf, &t1);
	/* Insertion of text may have invalidated t0, so recompute: */
	t0 = t1;
	gtk_text_iter_backward_chars(&t0, len);
	if (tagnames)
	{
		char **tag = tagnames;
		while (*tag)
		{
			gtk_text_buffer_apply_tag_by_name(tbuf, *tag, &t0, &t1);
			tag++;
		}
	}
	if (!ensurenewline)
		return;
	gtk_text_buffer_add_mark(tbuf, mark, &t1);
	gtk_text_view_scroll_to_mark(tview, mark, 0.0, 0, 0.0, 0.0);
	gtk_text_buffer_delete_mark(tbuf, mark);
}

static void sendMessage(GtkWidget *w /* <-- msg entry widget */, gpointer /* data */)
{
	char* tags[2] = {"self",NULL};
	tsappend("me: ",tags,0);
	GtkTextIter mstart; /* start of message pointer */
	GtkTextIter mend;   /* end of message pointer */
	gtk_text_buffer_get_start_iter(mbuf,&mstart);
	gtk_text_buffer_get_end_iter(mbuf,&mend);
	char* message = gtk_text_buffer_get_text(mbuf,&mstart,&mend,1);
	size_t len = g_utf8_strlen(message,-1);

	/* Encrypt message */
	unsigned char ct[512];
	memset(ct,0,512);
	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (1!=EVP_EncryptInit_ex(ctx,EVP_aes_256_ctr(),0,sharedSecret,sharedSecret+32))
		error("encryption failed");
	int nWritten; /* stores number of written bytes (size of ciphertext) */
	if (1!=EVP_EncryptUpdate(ctx,ct,&nWritten,(unsigned char*)message,len))
		error("encryption failed");
	EVP_CIPHER_CTX_free(ctx);
	size_t ctlen = nWritten;
	printf("ciphertext of length %i:\n",nWritten);
	for (size_t i = 0; i < ctlen; i++) {
		printf("%02x",ct[i]);
	}
	printf("\n");

	/* XXX we should probably do the actual network stuff in a different
	 * thread and have it call this once the message is actually sent. */
	ssize_t nbytes;
	if ((nbytes = send(sockfd,ct,nWritten,0)) == -1)
		error("send failed");

	tsappend(message,NULL,1);
	free(message);
	/* clear message text and reset focus */
	gtk_text_buffer_delete(mbuf,&mstart,&mend);
	gtk_widget_grab_focus(w);
}

static gboolean shownewmessage(gpointer msg)
{
	char *tags[2] = {"friend", NULL};
	char *friendname = "mr. friend: ";
	tsappend(friendname, tags, 0);
	char *message = (char *)msg;
	tsappend(message, NULL, 1);
	free(message);
	return 0;
}

int main(int argc, char *argv[])
{
	if (init("params") != 0)
	{
		fprintf(stderr, "could not read DH params from file 'params'\n");
		return 1;
	}
	// define long options
	static struct option long_opts[] = {
		{"connect", required_argument, 0, 'c'},
		{"listen", no_argument, 0, 'l'},
		{"port", required_argument, 0, 'p'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}};
	// process options:
	char c;
	int opt_index = 0;
	int port = 1337;
	char hostname[HOST_NAME_MAX + 1] = "localhost";
	hostname[HOST_NAME_MAX] = 0;

	while ((c = getopt_long(argc, argv, "c:lp:h", long_opts, &opt_index)) != -1)
	{
		switch (c)
		{
		case 'c':
			if (strnlen(optarg, HOST_NAME_MAX))
				strncpy(hostname, optarg, HOST_NAME_MAX);
			break;
		case 'l':
			isclient = 0;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'h':
			printf(usage, argv[0]);
			return 0;
		case '?':
			printf(usage, argv[0]);
			return 1;
		}
	}
	/* NOTE: might want to start this after gtk is initialized so you can
	 * show the messages in the main window instead of stderr/stdout.  If
	 * you decide to give that a try, this might be of use:
	 * https://docs.gtk.org/gtk4/func.is_initialized.html */
	if (isclient)
	{
		initClientNet(hostname, port);
		generateLongTermKey("clientKey");
		

	}
	else
	{
		initServerNet(port);
		generateLongTermKey("serverKey");
	}

	/* setup GTK... */
	GtkBuilder *builder;
	GObject *window;
	GObject *button;
	GObject *transcript;
	GObject *message;
	GError *error = NULL;
	gtk_init(&argc, &argv);
	builder = gtk_builder_new();
	if (gtk_builder_add_from_file(builder, "layout.ui", &error) == 0)
	{
		g_printerr("Error reading %s\n", error->message);
		g_clear_error(&error);
		return 1;
	}
	mark = gtk_text_mark_new(NULL, TRUE);
	window = gtk_builder_get_object(builder, "window");
	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	transcript = gtk_builder_get_object(builder, "transcript");
	tview = GTK_TEXT_VIEW(transcript);
	message = gtk_builder_get_object(builder, "message");
	tbuf = gtk_text_view_get_buffer(tview);
	mbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(message));
	button = gtk_builder_get_object(builder, "send");
	g_signal_connect_swapped(button, "clicked", G_CALLBACK(sendMessage), GTK_WIDGET(message));
	gtk_widget_grab_focus(GTK_WIDGET(message));
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_path(css, "colors.css", NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
											  GTK_STYLE_PROVIDER(css),
											  GTK_STYLE_PROVIDER_PRIORITY_USER);

	/* setup styling tags for transcript text buffer */
	gtk_text_buffer_create_tag(tbuf, "status", "foreground", "#657b83", "font", "italic", NULL);
	gtk_text_buffer_create_tag(tbuf, "friend", "foreground", "#6c71c4", "font", "bold", NULL);
	gtk_text_buffer_create_tag(tbuf, "self", "foreground", "#268bd2", "font", "bold", NULL);

	/* start receiver thread: */
	if (pthread_create(&trecv, 0, recvMsg, 0))
	{
		fprintf(stderr, "Failed to create update thread.\n");
	}

	gtk_main();

	shutdownNetwork();
	return 0;
}

/* thread function to listen for new messages and post them to the gtk
 * main loop for processing: */
void *recvMsg(void *)
{
	size_t maxlen = 512;
	char msg[maxlen+2]; /* might add \n and \0 */
	ssize_t nbytes;
	while (1) {
		if ((nbytes = recv(sockfd,msg,maxlen,0)) == -1)
			error("recv failed");
		if (nbytes == 0) {
			/* XXX maybe show in a status message that the other
			 * side has disconnected. */
			return 0;
		}
		char* m = malloc(maxlen+1);
		memcpy(m,msg,nbytes);
		if (m[nbytes-2] != '\n')
			m[nbytes++] = '\n';
		m[nbytes] = 0;

		/* Decrpyting message */
		unsigned char pt[512];
		memset(pt,0,512);
		EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
		if (1!=EVP_DecryptInit_ex(ctx,EVP_aes_256_ctr(),0,sharedSecret,sharedSecret+32))
			error("decryption failed");
		int nWritten; /* stores number of written bytes (size of ciphertext) */
		if (1!=EVP_DecryptUpdate(ctx,pt,&nWritten,(unsigned char*)m,nbytes))
			error("decryption failed");
		size_t ptlen = nWritten;
		printf("decrypted %lu bytes:\n%s\n",ptlen,pt);

		char* message = (char *)malloc(ptlen);
		memcpy(message,pt,ptlen);
			
		g_main_context_invoke(NULL,shownewmessage,(gpointer)message);
	}
	return 0;
}
