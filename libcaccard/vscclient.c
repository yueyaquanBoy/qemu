#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

#include "vscard_common.h"

#include "vreader.h"
#include "vcard_emul.h"
#include "vevent.h"
#include "passthru.h"

#include "mutex.h"

int verbose = 0;

int sock;

void
PrintByteArray (
    uint8_t *arrBytes,
    unsigned int nSize
) {
    int i;
    for (i=0; i < nSize; i++) {
        printf ("%02X ", arrBytes[i]);
    }
    printf ("\n");
}

void
PrintUsage () {
    printf ("vscclient [-c <certname> .. -e <emul_args> -d <level>%s] "
            "<host> <port> \n",
#ifdef USE_PASSTHRU
    " -p");
    printf (" -p use passthrough mode\n");
#else
   "");
#endif
    vcard_emul_usage();
}

char*
ip_numeric_to_char(
    uint32_t ip
) {
    char buf[4*4];

    sprintf(buf, "%d.%d.%d.%d", (ip & 0xff000000) >> 24, (ip & 0xff0000) >> 16,
        (ip & 0xff00) >> 8, ip & 0xff);
    return strdup(buf);
}

static mutex_t write_lock;

int
SendMsg (
    VSCMsgType type,
    uint32_t reader_id,
    const void *msg,
    unsigned int length
) {
    LONG rv;
    VSCMsgHeader mhHeader;

    MUTEX_LOCK(write_lock);

    if (verbose > 10) {
        printf("sending type=%d id=%d, len =%d (0x%x)\n",
               type, reader_id, length, length);
    }

    mhHeader.type = type;
    mhHeader.reader_id = 0;
    mhHeader.length = length;
    rv = write (
        sock,
        &mhHeader,
        sizeof (mhHeader)
    );
    if (rv < 0) {
        /* Error */
        printf ("write header error\n");
        close (sock);
        MUTEX_UNLOCK(write_lock);
        return (16);
    }
    rv = write (
        sock,
        msg,
        length
    );
    if (rv < 0) {
        /* Error */
        printf ("write error\n");
        close (sock);
        MUTEX_UNLOCK(write_lock);
        return (16);
    }
    MUTEX_UNLOCK(write_lock);

    return (0);
}

static VReader *pending_reader = NULL;
static mutex_t pending_reader_lock;
static condition_t pending_reader_condition;

#define MAX_ATR_LEN 40
static void *
event_thread(void *arg)
{
    unsigned char atr[ MAX_ATR_LEN];
    int atr_len = MAX_ATR_LEN;
    VEvent *event = NULL;
    unsigned int reader_id;


    while (1) {
        const char *reader_name;

        event = vevent_wait_next_vevent();
        if (event == NULL) {
            break;
        }
        reader_id = vreader_get_id(event->reader);
        if (reader_id == VSCARD_UNDEFINED_READER_ID &&
            event->type != VEVENT_READER_INSERT) {
            /* ignore events from readers qemu has rejected */
            /* if qemu is still deciding on this reader, wait to see if need to
             * forward this event */
            MUTEX_LOCK(pending_reader_lock);
            if (!pending_reader || (pending_reader != event->reader)) {
                /* wasn't for a pending reader, this reader has already been
                 * rejected by qemu */
                MUTEX_UNLOCK(pending_reader_lock);
                vevent_delete(event);
                continue;
            }
            /* this reader hasn't been told it's status from qemu yet, wait for
             * that status */
            while (pending_reader != NULL) {
                CONDITION_WAIT(pending_reader_condition,pending_reader_lock);
            }
            MUTEX_UNLOCK(pending_reader_lock);
            /* now recheck the id */
            reader_id = vreader_get_id(event->reader);
            if (reader_id == VSCARD_UNDEFINED_READER_ID) {
                /* this reader was rejected */
                vevent_delete(event);
                continue;
            }
            /* reader was accepted, now forward the event */
        }
        switch (event->type) {
        case VEVENT_READER_INSERT:
            /* tell qemu to insert a new CCID reader */
            /* wait until qemu has responded to our first reader insert
             * before we send a second. That way we won't confuse the responses
             * */
            MUTEX_LOCK(pending_reader_lock);
            while (pending_reader != NULL) {
                CONDITION_WAIT(pending_reader_condition,pending_reader_lock);
            }
            pending_reader = vreader_reference(event->reader);
            MUTEX_UNLOCK(pending_reader_lock);
            reader_name = vreader_get_name(event->reader);
            if (verbose > 10) {
                printf (" READER INSERT: %s\n", reader_name);
            }
            SendMsg (
                VSC_ReaderAdd,
                reader_id, /* currerntly VSCARD_UNDEFINED_READER_ID */
                NULL, 0
                /*reader_name,
                strlen(reader_name) */
            );

            break;
        case VEVENT_READER_REMOVE:
            /* future, tell qemu that an old CCID reader has been removed */
            if (verbose > 10) {
                printf (" READER REMOVE: %d \n", reader_id);
            }
            SendMsg(
                VSC_ReaderRemove,
                reader_id,
                NULL,
                0
            );
            break;
        case VEVENT_CARD_INSERT:
            /* get the ATR (intended as a response to a power on from the
             * reader */
            atr_len = MAX_ATR_LEN;
            vreader_power_on(event->reader, atr, &atr_len);
            /* ATR call functions as a Card Insert event */
            if (verbose > 10) {
                printf (" CARD INSERT %d: ", reader_id);
                PrintByteArray (atr, atr_len);
            }
            SendMsg (
                VSC_ATR,
                reader_id,
                atr,
                atr_len
            );
            break;
        case VEVENT_CARD_REMOVE:
            // Card removed
            if (verbose > 10) {
                printf (" CARD REMOVE %d: \n", reader_id);
            }
            SendMsg (
                VSC_CardRemove,
                reader_id,
                NULL,
                0
            );
            break;
        default:
            break;
        }
        vevent_delete(event);
    }
    return NULL;
}


unsigned int
get_id_from_string(char *string, unsigned int default_id)
{
    unsigned int id = atoi(string);

    /* don't accidentally swith to zero because no numbers have been supplied */
    if ((id == 0) && *string != '0') {
        return default_id;
    }
    return id;
}

void
do_command(void)
{
    char inbuf[255];
    char *string;
    VCardEmulError error;
    static unsigned int default_reader_id = 0;
    unsigned int reader_id;
    VReader *reader = NULL;

    reader_id = default_reader_id;
    string = fgets(inbuf, sizeof(inbuf), stdin);
    if (string != NULL) {
        if (strncmp(string,"exit",4) == 0) {
            /* remove all the readers */
            VReaderList *list = vreader_get_reader_list();
            VReaderListEntry *reader_entry;
            printf("Active Readers:\n");
            for (reader_entry = vreader_list_get_first(list); reader_entry;
                 reader_entry = vreader_list_get_next(reader_entry)) {
                VReader *reader = vreader_list_get_reader(reader_entry);
                VReaderID reader_id;
                reader_id=vreader_get_id(reader);
                if (reader_id == -1) {
                    continue;
                }
                /* be nice and signal card removal first (qemu probably should
                 * do this itself) */
                if (vreader_card_is_present(reader) == VREADER_OK) {
                    SendMsg (
                        VSC_CardRemove,
                        reader_id,
                        NULL,
                        0
                    );
                }
                SendMsg (
                    VSC_ReaderRemove,
                    reader_id,
                    NULL,
                    0
                );
            }
            exit(0);
        } else if (strncmp(string,"insert",6) == 0) {
            if (string[6] == ' ') {
                reader_id = get_id_from_string(&string[7], reader_id);
            }
            reader = vreader_get_reader_by_id(reader_id);
            error = vcard_emul_force_card_insert(reader);
            printf("insert %s, returned %d\n", reader ? vreader_get_name(reader)
                                               : "invalid reader", error);
        } else if (strncmp(string,"remove",6) == 0) {
            if (string[6] == ' ') {
                reader_id = get_id_from_string(&string[7], reader_id);
            }
            reader = vreader_get_reader_by_id(reader_id);
            error = vcard_emul_force_card_remove(reader);
            printf("remove %s, returned %d\n", reader ? vreader_get_name(reader)
                                               : "invalid reader", error);
        } else if (strncmp(string,"select",6) == 0) {
            if (string[6] == ' ') {
                reader_id = get_id_from_string(&string[7],
                                               VSCARD_UNDEFINED_READER_ID);
            }
            if (reader_id != VSCARD_UNDEFINED_READER_ID) {
                reader = vreader_get_reader_by_id(reader_id);
            }
            if (reader) {
                printf("Selecting reader %d, %s\n", reader_id,
                        vreader_get_name(reader));
                default_reader_id = reader_id;
            } else {
                printf("Reader with id %d not found\n", reader_id);
            }
        } else if (strncmp(string,"debug",5) == 0) {
            if (string[5] == ' ') {
                verbose = get_id_from_string(&string[6],0);
            }
            printf ("debug level = %d\n", verbose);
        } else if (strncmp(string,"list",4) == 0) {
            VReaderList *list = vreader_get_reader_list();
            VReaderListEntry *reader_entry;
            printf("Active Readers:\n");
            for (reader_entry = vreader_list_get_first(list); reader_entry;
                 reader_entry = vreader_list_get_next(reader_entry)) {
                VReader *reader = vreader_list_get_reader(reader_entry);
                VReaderID reader_id;
                reader_id=vreader_get_id(reader);
                if (reader_id == -1) {
                    continue;
                }
                printf("%3d %s %s\n",reader_id,
                       vreader_card_is_present(reader) == VREADER_OK ?
                       "CARD_PRESENT": "            ",
                       vreader_get_name(reader));
            }
            printf("Inactive Readers:\n");
            for (reader_entry = vreader_list_get_first(list); reader_entry;
                 reader_entry = vreader_list_get_next(reader_entry)) {
                VReader *reader = vreader_list_get_reader(reader_entry);
                VReaderID reader_id;
                reader_id=vreader_get_id(reader);
                if (reader_id != -1) {
                    continue;
                }

                printf("INA %s %s\n",
                       vreader_card_is_present(reader) == VREADER_OK ?
                       "CARD_PRESENT": "            ",
                       vreader_get_name(reader));
            }
        } else if (*string != 0) {
            printf("valid commands: \n");
            printf("insert [reader_id]\n");
            printf("remove [reader_id]\n");
            printf("select reader_id\n");
            printf("list\n");
            printf("debug [level]\n");
            printf("exit\n");
        }
    }
    vreader_free(reader);
    printf("> ");
    fflush(stdout);
}


#define APDUBufSize 270

// just for ease of parsing command line arguments.
#define MAX_CERTS 100

int
connect_to_qemu (
    const char *ip,
    uint32_t port
) {
    struct addrinfo hints;
    struct addrinfo* server;
    int ret;
    char port_str[10];

    sock = socket (
        AF_INET,
        SOCK_STREAM,
        0
    );
    if (sock < 0) {
        // Error
        printf ("Error opening socket!\n");
    }

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          /* Any protocol */
    snprintf(port_str, sizeof(port_str) - 1, "%d", port);

    ret = getaddrinfo(ip, port_str, &hints, &server);

    if (ret != 0) {
        printf ("getaddrinfo failed\n");
        return (5);
    }

    if (connect (
            sock,
            server->ai_addr,
            server->ai_addrlen
        ) < 0
    ) {
        // Error
        printf ("Could not connect\n");
        return (5);
    }
    if (verbose) {
        printf ("Connected (sizeof Header=%zd)!\n", sizeof (VSCMsgHeader));
    }
    return sock;
}

int
main (
    int argc,
    char *argv[]
) {
    char* qemu_ip;
    uint16_t qemu_port;
    VSCMsgHeader mhHeader;
    VSCMsgError *error_msg;

    LONG rv;
    int dwSendLength;
    int dwRecvLength;
    uint8_t pbRecvBuffer[APDUBufSize];
    uint8_t pbSendBuffer[APDUBufSize];
     VReaderStatus reader_status;
    VReader *reader = NULL;
    VCardEmulOptions *command_line_options = NULL;
    pthread_t thread_id;
    int passthru = 0;

    char* cert_names[MAX_CERTS];
    char* emul_args = NULL;
    int cert_count = 0;
    int c;

    while ((c = getopt(argc, argv, "c:e:pd:")) != -1) {
        switch (c) {
            case 'c':
                if (cert_count >= MAX_CERTS) {
                    printf("too many certificates (max = %d)\n", MAX_CERTS);
                    exit (5);
                }
                cert_names[cert_count++] = optarg;
                break;
            case 'e':
                emul_args = optarg;
                break;
            case 'p':
#ifdef USE_PASSTHRU
                passthru = 1;
#else
                PrintUsage();
                exit(4);
#endif
                break;
            case 'd':
                verbose = get_id_from_string(optarg,1);
                break;
        }
    }

    if (argc - optind != 2) {
        PrintUsage();
        exit (4);
    }

    if (!passthru && cert_count > 0) {
        char *new_args;
        int len, i;
        /* if we've given some -c options, we clearly we want do so some
         * software emulation.  add that emulation now. this is NSS Emulator
         * specific */
        if (emul_args == NULL) {
            emul_args = "db=\"/etc/pki/nssdb\"";
        }
#define SOFT_STRING ",soft=(,Virtual Reader,CAC,,"
             /* 2 == close paren & null */
        len = strlen(emul_args) + strlen(SOFT_STRING) + 2;
        for (i=0; i < cert_count; i++) {
            len +=strlen(cert_names[i])+1; /* 1 == comma */
        }
        new_args = malloc(len);
        strcpy(new_args,emul_args);
        strcat(new_args,SOFT_STRING);
        for (i=0; i < cert_count; i++) {
            strcat(new_args,cert_names[i]);
            strcat(new_args,",");
        }
        strcat(new_args,")");
        emul_args = new_args;
    }
    if (emul_args) {
#ifdef USE_PASSTHRU
        command_line_options = passthru ? passthru_emul_options(emul_args) :
#else
        command_line_options =
#endif
                                          vcard_emul_options(emul_args);
    }

    qemu_ip = strdup(argv[argc - 2]);
    qemu_port = (uint16_t)atoi(argv[argc -1]);
    sock = connect_to_qemu(qemu_ip, qemu_port);

    /* remove whatever reader might be left in qemu,
     * in case of a unclean previous exit. */
    SendMsg(
        VSC_ReaderRemove,
        VSCARD_MINIMAL_READER_ID,
        NULL,
        0
    );

    MUTEX_INIT(write_lock);
    MUTEX_INIT(pending_reader_lock);
    CONDITION_INIT(pending_reader_condition);

#ifdef USE_PASSTHRU
    if (passthru) {
        passthru_emul_init(command_line_options);
    } else
#endif
        vcard_emul_init(command_line_options);

    /* launch the event_thread. This will trigger reader adds for all the
     * existing readers */
    rv = pthread_create(&thread_id, NULL, event_thread, reader);
    if (rv < 0) {
        perror("pthread_create");
        exit (1);
    }

    printf("> ");
    fflush(stdout);

    do {
        fd_set fds;

        FD_ZERO(&fds);
        FD_SET(1,&fds);
        FD_SET(sock,&fds);

        /* waiting on input from the socket */
        rv = select(sock+1, &fds, NULL, NULL, NULL);
        if (rv < 0) {
            /* handle error */
            perror("select");
            return (7);
        }
        if (FD_ISSET(1,&fds)) {
            do_command();
        }
        if (!FD_ISSET(sock,&fds)) {
            continue;
        }

        rv = read (
            sock,
            &mhHeader,
            sizeof (mhHeader)
        );
        if (rv < sizeof(mhHeader)) {
            /* Error */
            if (rv < 0) {
                perror("header read error\n");
            } else {
                printf ("header short read %ld\n", rv);
            }
            return (8);
        }
        if (verbose) {
            printf ("Header: type=%d, reader_id=%d length=%d (0x%x)\n",
                    mhHeader.type, mhHeader.reader_id, mhHeader.length,
                                               mhHeader.length);
        }
        switch (mhHeader.type) {
            case VSC_APDU:
                rv = read (
                    sock,
                    pbSendBuffer,
                    mhHeader.length
                );
                if (rv < 0) {
                    /* Error */
                    printf ("read error\n");
                    close (sock);
                    return (8);
                }
                if (verbose) {
                    printf (" recv APDU: ");
                    PrintByteArray (pbSendBuffer, mhHeader.length);
                }
                /* Transmit recieved APDU */
                dwSendLength = mhHeader.length;
                dwRecvLength = sizeof(pbRecvBuffer);
                reader = vreader_get_reader_by_id(mhHeader.reader_id);
                reader_status = vreader_xfr_bytes(reader,
                    pbSendBuffer, dwSendLength,
                    pbRecvBuffer, &dwRecvLength);
                if (reader_status == VREADER_OK) {
                    mhHeader.length = dwRecvLength;
                if (verbose) {
                    printf (" send response: ");
                    PrintByteArray (pbRecvBuffer, mhHeader.length);
                }
                    SendMsg (
                        VSC_APDU,
                        mhHeader.reader_id,
                        pbRecvBuffer,
                        dwRecvLength
                    );
                } else {
                       rv = reader_status; /* warning: not meaningful */
                    SendMsg (
                        VSC_Error,
                        mhHeader.reader_id,
                        &rv,
                        sizeof (LONG)
                    );
                }
                vreader_free(reader);
                reader = NULL; /* we've freed it, don't use it by accident
                                  again */
                break;
            case VSC_Reconnect:
                {
                    VSCMsgReconnect reconnect;

                    if (read(sock, (char*)&reconnect, mhHeader.length) < 0) {
                        printf ("read error\n");
                        close (sock);
                        return (8);
                    }
                    if (reconnect.ip != 0) {
                        reconnect.ip = ntohl(reconnect.ip);
                        free(qemu_ip);
                        qemu_ip = ip_numeric_to_char(reconnect.ip);
                        qemu_port = reconnect.port;
                    } else {
                        printf("info: reconnect with no target ip:port: "
                               "bumping port by one and reconnecting\n");
                        qemu_port = qemu_port + 1;
                    }
                    /* sent when qemu is migrating, we need to close the socket
                     * and reconnect. */
                    close(sock);
                    printf("reconnecting to %s:%d\n", qemu_ip, qemu_port);
                    sock = connect_to_qemu(qemu_ip, qemu_port);
                }
                break;
            case VSC_ReaderAddResponse:
               MUTEX_LOCK(pending_reader_lock);
                if (pending_reader) {
                    vreader_set_id(pending_reader, mhHeader.reader_id);
                    vreader_free(pending_reader);
                    pending_reader = NULL;
                    CONDITION_NOTIFY(pending_reader_condition);
                }
                MUTEX_UNLOCK(pending_reader_lock);
                break;
            case VSC_Error:
                rv = read (
                    sock,
                    pbSendBuffer,
                    mhHeader.length
                );
                error_msg = (VSCMsgError *) pbSendBuffer;
                printf("error: qemu refused to add reader\n");
                if (error_msg->code == VSC_CANNOT_ADD_MORE_READERS) {
                    /* clear pending reader, qemu can't handle any more */
                    MUTEX_LOCK(pending_reader_lock);
                    if (pending_reader) {
                        pending_reader = NULL;
                        /* make sure the event loop doesn't hang */
                        CONDITION_NOTIFY(pending_reader_condition);
                    }
                    MUTEX_UNLOCK(pending_reader_lock);
                }
                break;
            default:
                printf ("Default\n");
                return 0;
        }
    } while (rv >= 0);


    return (0);
}
