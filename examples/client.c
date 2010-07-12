/* coap-client -- simple CoAP client
 *
 * (c) 2010 Olaf Bergmann <bergmann@tzi.org>
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "coap.h"

static coap_list_t *optlist = NULL;
/* Request URI.
 * TODO: associate the resources with transaction id and make it expireable */
static coap_uri_t uri;

extern unsigned int
print_readable( const unsigned char *data, unsigned int len, 
		unsigned char *result, unsigned int buflen );

coap_pdu_t *
new_ack( coap_context_t  *ctx, coap_queue_t *node ) {
  coap_pdu_t *pdu = coap_new_pdu();
  
  if (pdu) {
    pdu->hdr->type = COAP_MESSAGE_ACK;
    pdu->hdr->code = 0;
    pdu->hdr->id = node->pdu->hdr->id;
  }

  return pdu;
}

coap_pdu_t *
new_response( coap_context_t  *ctx, coap_queue_t *node, unsigned int code ) {
  coap_pdu_t *pdu = new_ack(ctx, node);

  if (pdu)
    pdu->hdr->code = code;

  return pdu;
}

coap_pdu_t *
coap_new_get( coap_list_t *options ) {
  coap_pdu_t *pdu;
  coap_list_t *opt;
  static unsigned char buf[201];
  if ( ! ( pdu = coap_new_pdu() ) )
    return NULL;

  pdu->hdr->type = COAP_MESSAGE_CON;
  pdu->hdr->code = COAP_REQUEST_GET;

  for (opt = optlist; opt; opt = opt->next) {
    debug("add option %u of length %u\n", COAP_OPTION_KEY(*(coap_option *)opt->data),COAP_OPTION_LENGTH(*(coap_option *)opt->data));
    buf[print_readable( COAP_OPTION_DATA(*(coap_option *)opt->data), COAP_OPTION_LENGTH(*(coap_option *)opt->data),
		    buf, 200)]=0;
    printf("%s\n",buf);
    coap_add_option( pdu, COAP_OPTION_KEY(*(coap_option *)opt->data), 
		     COAP_OPTION_LENGTH(*(coap_option *)opt->data), 
		     COAP_OPTION_DATA(*(coap_option *)opt->data) );
  }
  
  return pdu;
}

void 
send_request( coap_context_t  *ctx, coap_pdu_t  *pdu, const char *server, unsigned short port ) {
  struct addrinfo *res, *ainfo;
  struct addrinfo hints;
  int error;
  struct sockaddr_in6 dst;
  static unsigned char buf[COAP_MAX_PDU_SIZE];
  memset ((char *)&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_family = AF_INET6;

  error = getaddrinfo(server, "", &hints, &res);

  if (error != 0) {
    perror("getaddrinfo");
    exit(1);
  }

  for (ainfo = res; ainfo != NULL; ainfo = ainfo->ai_next) {

    if ( ainfo->ai_family == AF_INET6 ) {

      memset(&dst, 0, sizeof dst );
      dst.sin6_family = AF_INET6;
      dst.sin6_port = htons( port );
      memcpy( &dst.sin6_addr, &((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_addr, sizeof(dst.sin6_addr) );

      print_readable( (unsigned char *)pdu->hdr, pdu->length, buf, COAP_MAX_PDU_SIZE);
      printf("%s\n",buf);
      coap_send_confirmed( ctx, &dst, pdu );
      goto leave;
    }
  }
 
 leave:
  freeaddrinfo(res);
}

#define COAP_OPT_BLOCK_LAST(opt) ( COAP_OPT_VALUE(*block) + (COAP_OPT_LENGTH(*block) - 1) )
#define COAP_OPT_BLOCK_MORE(opt) ( *COAP_OPT_LAST(*block) & 0x08 )
#define COAP_OPT_BLOCK_SIZE(opt) ( *COAP_OPT_LAST(*block) & 0x07 )
  
unsigned int  
_read_blk_nr(coap_opt_t *opt) {
  unsigned int i, nr=0;
  for ( i = COAP_OPT_LENGTH(*opt); i; --i) {
    nr = (nr << 8) + COAP_OPT_VALUE(*opt)[i-1];
  }
  return nr >> 4;
}
#define COAP_OPT_BLOCK_NR(opt)   _read_blk_nr(&opt)


void 
message_handler( coap_context_t  *ctx, coap_queue_t *node, void *data) {
  coap_pdu_t *pdu = NULL;
  coap_opt_t *block, *ct;
  unsigned int blocknr;
  unsigned char buf[4];
  coap_list_t *option;

#ifndef NDEBUG
  printf("** process pdu: ");
  coap_show_pdu( node->pdu );
#endif

  if ( node->pdu->hdr->version != COAP_DEFAULT_VERSION ) {
    debug("dropped packet with unknown version %u\n", node->pdu->hdr->version);
    return;
  }
    
  if ( node->pdu->hdr->code < COAP_RESPONSE_100 && node->pdu->hdr->type == COAP_MESSAGE_CON ) {
    /* send 500 response */
    pdu = new_response( ctx, node, COAP_RESPONSE_500 );
    goto finish;
  }

  switch (node->pdu->hdr->code) {
  case COAP_RESPONSE_200:
    /* got some data, check if block option is set */
    block = coap_check_option( node->pdu, COAP_OPTION_BLOCK );
    if ( !block ) {
      ;
    } else {
      blocknr = coap_decode_var_bytes( COAP_OPT_VALUE(*block), COAP_OPT_LENGTH(*block) );
      if ( (blocknr & 0x08) ) { 
	/* more bit is set */
	printf("found the M bit, block size is %u, block nr. %u\n",
	       blocknr & 0x07, 
	       (blocknr & 0xf0) << blocknr & 0x07);
	
	/* need to acknowledge if message was asyncronous */
	if ( node->pdu->hdr->type == COAP_MESSAGE_CON ) {
	  pdu = new_ack( ctx, node );      
	  
	  if ( pdu && coap_send( ctx, &node->remote, pdu ) == COAP_INVALID_TID ) {
	    debug("message_handler: error sending reponse");
	    coap_delete_pdu(pdu);
	    return;
	  }  
	}
	
	/* create pdu with request for next block */
	pdu = coap_new_get( NULL ); /* first, create bare PDU w/o any option  */
	if ( pdu ) {
	  pdu->hdr->id = node->pdu->hdr->id; /* copy transaction id from response */
	  
	  /* get content type from response */
	  ct = coap_check_option( node->pdu, COAP_OPTION_CONTENT_TYPE );
	  if ( ct ) {
	    coap_add_option( pdu, COAP_OPTION_CONTENT_TYPE, 
			     COAP_OPT_LENGTH(*ct),COAP_OPT_VALUE(*ct) );
	  }

	  /* add URI components from optlist */
	  for (option = optlist; option; option = option->next ) {
	    switch (COAP_OPTION_KEY(*(coap_option *)option)) {
	    case COAP_OPTION_URI_SCHEME :
	    case COAP_OPTION_URI_AUTHORITY :
	    case COAP_OPTION_URI_PATH :
	      coap_add_option ( pdu, COAP_OPTION_KEY(*(coap_option *)option), 
				COAP_OPTION_LENGTH(*(coap_option *)option),
				COAP_OPTION_DATA(*(coap_option *)option) );
	      break;
	    default:
	      ;			/* skip other options */
	    }
	  }
	  
	  /* finally add updated block option from response */
	  coap_add_option ( pdu, COAP_OPTION_BLOCK, 
			    coap_encode_var_bytes(buf, blocknr + ( 1 << 4) ), buf);
	  	  
	  if ( coap_send_confirmed( ctx, &node->remote, pdu ) == COAP_INVALID_TID ) {
	    debug("message_handler: error sending reponse");
	    coap_delete_pdu(pdu);
	  }
	  return;
	}      
      }
    }
    break;
  default:
    /* acknowledge if requested */
    if ( node->pdu->hdr->type == COAP_MESSAGE_CON ) {
      pdu = new_ack( ctx, node );
    }
  }

  finish:
  if ( pdu && coap_send( ctx, &node->remote, pdu ) == COAP_INVALID_TID ) {
    debug("message_handler: error sending reponse");
    coap_delete_pdu(pdu);
  }  
}

void 
usage( const char *program, const char *version) {
  const char *p;

  p = strrchr( program, '/' );
  if ( p )
    program = ++p;

  fprintf( stderr, "%s v%s -- a small CoAP implementation\n"
	   "(c) 2010 Olaf Bergmann <bergmann@tzi.org>\n\n"
	   "usage: %s [-b num] [-c type...] [-g group] [-p port] URI\n\n"
	   "\tURI can be an absolute or relative coap URI,\n"
	   "\t-b size\t\tblock size to be used in GET/PUT/POST requests\n"
	   "\t       \t\t(value must be a mulitple of 16 not larger than 2048)\n"
	   "\t-c type\t\taccepted content type (multiple occurrences allowed)\n"
	   "\t-g group\tjoin the given multicast group\n"
	   "\t-p port\t\tlisten on specified port\n",
	   program, version, program );
}

int
join( coap_context_t *ctx, char *group_name ){
  struct ipv6_mreq mreq;
  struct addrinfo   *reslocal = NULL, *resmulti = NULL, hints, *ainfo;
  int result = -1;

  /* we have to resolve the link-local interface to get the interface id */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;

  result = getaddrinfo("::", NULL, &hints, &reslocal);
  if ( result < 0 ) {
    perror("join: cannot resolve link-local interface");
    goto finish;
  }

  /* get the first suitable interface identifier */
  for (ainfo = reslocal; ainfo != NULL; ainfo = ainfo->ai_next) {
    if ( ainfo->ai_family == AF_INET6 ) {
      mreq.ipv6mr_interface = 
	      ((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_scope_id;
      break;
    }
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;

  /* resolve the multicast group address */
  result = getaddrinfo(group_name, NULL, &hints, &resmulti);

  if ( result < 0 ) {
    perror("join: cannot resolve multicast address");
    goto finish;
  }

  for (ainfo = resmulti; ainfo != NULL; ainfo = ainfo->ai_next) {
    if ( ainfo->ai_family == AF_INET6 ) {
      mreq.ipv6mr_multiaddr = 
	((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_addr;
      break;
    }
  }
  
  result = setsockopt( ctx->sockfd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
		       (char *)&mreq, sizeof(mreq) );
  if ( result < 0 ) 
    perror("join: setsockopt");

 finish:
  freeaddrinfo(resmulti);
  freeaddrinfo(reslocal);
  
  return result;
}

int 
order_opts(void *a, void *b) {
  if (!a || !b)
    return a < b ? -1 : 1;

  return (COAP_OPTION_KEY(*(coap_option *)a) < COAP_OPTION_KEY(*(coap_option *)b))
    ? -1 
    : 1;
}

coap_list_t *
new_option_node(unsigned short key, unsigned int length, unsigned char *data) {
  coap_option *option;
  coap_list_t *node;

  option = coap_malloc(sizeof(coap_option) + length);
  if ( !option ) 
    goto error;
    
  COAP_OPTION_KEY(*option) = key;
  COAP_OPTION_LENGTH(*option) = length;
  memcpy(COAP_OPTION_DATA(*option), data, length);
  
  node = coap_new_listnode(option);
  if ( node ) 
    return node;

 error:
  perror("new_option_node: malloc");
  coap_free( option );
  return NULL;  
}

void
cmdline_content_type(char *arg) {
  static char *content_types[] = 
    { "plain", "xml", "csv", "html", "","","","","","","","","","","","","","","","","",
      "gif", "jpeg", "png", "tiff", "audio", "video", "","","","","","","","","","","","","",
      "link", "axml", "binary", "rdf", "soap", "atom", "xmpp", "exi",
      "bxml", "infoset", "json", 0};
  coap_list_t *node;
  unsigned char i;

  for (i=0; content_types[i] && strncmp(arg,content_types[i],strlen(arg)) != 0 ; ++i) 
    ;
  
  if ( content_types[i] ) {
    node = new_option_node(COAP_OPTION_CONTENT_TYPE, 1, (unsigned char *)&i);


    if ( node ) 
      coap_insert( &optlist, node, order_opts );

  } else {
    fprintf(stderr, "W: unknown content-type '%s'\n",arg);
  }
}

void
cmdline_uri(char *arg) {
  coap_split_uri( arg, &uri );

  if (uri.scheme)
    coap_insert( &optlist, new_option_node(COAP_OPTION_URI_SCHEME, 
					   strlen(uri.scheme), (unsigned char *)uri.scheme),
					   order_opts);

  if (uri.na)
    coap_insert( &optlist, new_option_node(COAP_OPTION_URI_AUTHORITY, 
					   strlen(uri.na), (unsigned char *)uri.na),
					   order_opts);

  if (uri.path)
    coap_insert( &optlist, new_option_node(COAP_OPTION_URI_PATH, 
					   strlen(uri.path), (unsigned char *)uri.path),
					   order_opts);
}

void
cmdline_blocksize(char *arg) {
  static unsigned char buf[4];	/* hack: temporarily take encoded bytes */
  unsigned int blocksize = atoi(arg);
  
  if ( COAP_MAX_PDU_SIZE < blocksize + sizeof(coap_hdr_t) ) {
    fprintf(stderr, "W: skipped invalid blocksize\n");
    return;
  }

  /* use only last three bits and clear M-bit */
  blocksize = (coap_fls(blocksize >> 4) - 1) & 0x07; 
  coap_insert( &optlist, new_option_node(COAP_OPTION_BLOCK, 
					 coap_encode_var_bytes(buf, blocksize), buf),
					 order_opts);
}

int 
main(int argc, char **argv) {
  coap_context_t  *ctx;
  fd_set readfds;
  struct timeval tv, *timeout;
  int result;
  time_t now;
  coap_queue_t *nextpdu;
  coap_pdu_t  *pdu;
  static char *server = NULL, *p;
  unsigned short localport = COAP_DEFAULT_PORT, port = COAP_DEFAULT_PORT;
  int opt;
  char *group = NULL;

  while ((opt = getopt(argc, argv, "b:c:g:p:")) != -1) {
    switch (opt) {
    case 'b' :
      cmdline_blocksize(optarg);
      break;
    case 'c' :
      cmdline_content_type(optarg);
      break;
    case 'g' :
      group = optarg;
      break;
    case 'p' :
      localport = atoi(optarg);
      break;
    default:
      usage( argv[0], VERSION );
      exit( 1 );
    }
  }

  ctx = coap_new_context( localport );
  if ( !ctx )
    return -1;

  coap_register_message_handler( ctx, message_handler );

  if ( optind < argc )
    cmdline_uri( argv[optind] );
  else {
    usage( argv[0], VERSION );
    exit( 1 );
  }

  if ( group )
    join( ctx, group );

  if (! (pdu = coap_new_get( optlist ) ) )
    return -1;

  /* split server address and port */
  /* FIXME: get rid of the global URI object somehow */
  server = uri.na;

  if (server) {
    if (*server == '[') {	/* IPv6 address reference */
      p = ++server;
      
      while ( *p && *p != ']' ) 
	++p;

      if (*p == ']')
	*p++ = '\0';		/* port starts here */
    } else {			/* IPv4 address or hostname */
      p = server;
      while ( *p && *p != ':' ) 
	++p;
    }
  
    if (*p == ':') {		/* port starts here */
      *p++ = '\0';
      port = 0;
      
      /* set port */
      while( isdigit(*p) ) {
	port = port * 10 + ( *p - '0' );
	++p;
      }
    }
  }

  /* send request */
  send_request( ctx, pdu, server ? server : "::1", port );

  while ( 1 ) {
    FD_ZERO(&readfds); 
    FD_SET( ctx->sockfd, &readfds );
    
    nextpdu = coap_peek_next( ctx );

    time(&now);
    while ( nextpdu && nextpdu->t <= now ) {
      coap_retransmit( ctx, coap_pop_next( ctx ) );
      nextpdu = coap_peek_next( ctx );
    }

    if ( nextpdu ) {	        /* set timeout if there is a pdu to send */
      tv.tv_usec = 0;
      tv.tv_sec = nextpdu->t - now;
      timeout = &tv;
    } else 
      timeout = NULL;		/* no timeout otherwise */

    result = select( ctx->sockfd + 1, &readfds, 0, 0, timeout );
    
    if ( result < 0 ) {		/* error */
      perror("select");
    } else if ( result > 0 ) {	/* read from socket */
      if ( FD_ISSET( ctx->sockfd, &readfds ) ) {
	coap_read( ctx );	/* read received data */
	coap_dispatch( ctx );	/* and dispatch PDUs from receivequeue */
      }
    }
  }

  coap_free_context( ctx );

  return 0;
}