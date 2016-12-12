#include "simple_message_client_commandline_handling.h"
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define v(fmt, ...)                                                                                          \
  if (verbose)                                                                                               \
    fprintf(stderr, "%s(): " fmt, __func__, __VA_ARGS__);

static int verbose = 0;

static void usage(FILE *stream, const char *cmd, int code);
static int connection(const char *server, const char *port);
static int request(FILE *write_fd, int sock, const char *user, const char *message, const char *img_url);
static int response(FILE *read_fd);
static int parse_string(char *line, const char *key, char *result);
static int parse_long(char *line, const char *key, long *result);

/**
 * @brief the entry point
 *
 * @param argc the number of arguments
 * @param argv the arguments
 *
 * @returns EXIT_SUCCESS, EXIT_FAILURE
 */
int main(int argc, const char *argv[]) {
  smc_usagefunc_t usagefunc = usage;
  const char *server = NULL;
  const char *port = NULL;
  const char *user = NULL;
  const char *message = NULL;
  const char *img_url = NULL;
  int sock = -1;
  FILE *write_fd = NULL;
  FILE *read_fd = NULL;
  int status = 1;

  smc_parsecommandline(argc, argv, usagefunc, &server, &port, &user, &message, &img_url, &verbose);
  v("server: %s, port: %s, user: %s, message: %s, img_url: %s\n", server, port, user, message, img_url);

  if ((sock = connection(server, port)) == -1) {
    /* error is printed by connection() */
    return EXIT_FAILURE;
  }

  if ((write_fd = fdopen(sock, "w")) == NULL) {
    warn("fdopen w");
    return EXIT_FAILURE;
  }

  if (request(write_fd, sock, user, message, img_url) == -1) {
    /* error is printed by request() */
    fclose(write_fd); /* also closes sock */
    return EXIT_FAILURE;
  }

  if ((read_fd = fdopen(sock, "r")) == NULL) {
    warn("fdopen r");
    return EXIT_FAILURE;
  }

  status = response(read_fd);

  fclose(write_fd);
  fclose(read_fd);

  return status;
}

/**
 * @brief prints the usage information
 *
 * @param stream where to print the message
 * @param cmd the executable name
 * @param code the exit code
 */
static void usage(FILE *stream, const char *cmd, int code) {
  (void)fprintf(stream, "Usage: %s -s server -p port -u user [-i image URL] -m message [-v] [-h]\n", cmd);
  exit(code);
}

/**
 * @brief initiates a connection to the server
 *
 * @param server the server address
 * @param port the server port
 *
 * @returns 0 if everything went well or -1 in case of error
 */
static int connection(const char *server, const char *port) {
  int sock = -1;
  struct addrinfo hints;
  struct addrinfo *info, *p;

  /* get the address info */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;     /* IPv4 and IPv6 */
  hints.ai_socktype = SOCK_STREAM; /* TCP */
  hints.ai_flags = AI_ADDRCONFIG;  /* Only use families present on system */

  if (getaddrinfo(server, port, &hints, &info) != 0) {
    warn("getaddrinfo");
    return -1;
  }

  /*
   * getaddrinfo() returns a list of address structures
   * try each address until we successfully connect
   */
  for (p = info; p != NULL; p = p->ai_next) {
    if ((sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      continue;
    }

    if (connect(sock, p->ai_addr, p->ai_addrlen) == -1) {
      warn("connect");
      close(sock);
      continue;
    } else {
      v("%s\n", "Connected");
      break; /* success */
    }
  }

  if (p == NULL) {
    warnx("Could not connect");
    freeaddrinfo(info);
    return -1;
  }

  freeaddrinfo(info);

  return sock;
}

/**
 * @brief sends the request to the server
 *
 * @param write_fd the stream descriptor
 * @param sock the socket identifier
 * @param user the user string
 * @param message the message string
 * @param img_url the img_url string
 *
 * @returns 0 if everything went well or -1 in case of error
 */
static int request(FILE *write_fd, int sock, const char *user, const char *message, const char *img_url) {

  /* img_url is optional */
  if (img_url == NULL) {
    img_url = "";
  }

  /* prefixes */
  char *user_p = "user=";
  char *img_url_p = img_url[0] ? "\nimg=" : "";
  char *message_p = "\n";

  size_t length = strlen(user_p) + strlen(img_url_p) + strlen(message_p) + \
                  strlen(user) + strlen(img_url) + strlen(message) + 1;

  char *request = calloc(length, sizeof(char));

  if (request == NULL) {
    warn("calloc");
    return -1;
  }

  snprintf(request, length, "%s%s%s%s%s%s", user_p, user, img_url_p, img_url, message_p, message);
  v("Request: %s\n", request);

  if (fprintf(write_fd, "%s", request) < 0) {
    warn("fprintf");
    free(request);
    return -1;
  }

  if (fflush(write_fd) != 0) {
    warn("fflush");
    free(request);
    return -1;
  }

  if (shutdown(sock, SHUT_WR)) {
    warn("shutdown");
    free(request);
    return -1;
  }

  free(request);

  return 0;
}

/**
 * @brief receives the response
 *
 * @param read_fd the stream descriptor
 *
 * @returns the status from the server or 1 in case of error
 */
static int response(FILE *read_fd) {
  char *line = NULL;
  ssize_t read = -1;
  size_t len = 0;
  int stage = 0;
  long status = 1;
  char file_name[NAME_MAX] = {0};
  long file_len = 0;
  int counter = 0;
  FILE *fp = NULL;

  while ((read = getline(&line, &len, read_fd)) != -1) {

    switch (stage) {

    case 0: /* get the status */
      if (parse_long(line, "status", &status) == -1) {
        break;
      }
      v("Status: %ld\n", status);
      stage++;
      continue;

    case 1: /* get the file name */
      if (parse_string(line, "file", file_name) == -1) {
        break;
      }
      v("File: %s\n", file_name);
      stage++;
      continue;

    case 2: /* get the file length and open it for writing */
      if (parse_long(line, "len", &file_len) == -1) {
        break;
      }
      if ((fp = fopen(file_name, "w+")) == NULL) {
        break;
      }
      v("File len: %ld\n", file_len);
      stage++;
      continue;

    case 3: /* write the file */
      counter += read;
      if (fputs(line, fp) == EOF) {
        warn("fputs");
        break;
      }
      v("Written: %d of %ld\n", counter, file_len);
      if (counter >= file_len) {
        if (fflush(fp) == EOF) {
          warn("fflush");
          break;
        }
        stage = 1; /* read next file */
        counter = 0;
        fclose(fp);
        fp = NULL;
      }
      continue;

    default:
      break;
    }

    warnx("Could not process the server response");
    break;
  }

  if (fp != NULL) {
    fclose(fp);
  }

  if (line != NULL) {
    free(line);
  }

  return (int)status;
}

/**
 * @brief parses the key value delimited by '=', as string
 *
 * @param line the string to parse
 * @param key the key to search for
 * @param result the value of the key
 *
 * @returns 0 if everything went well or -1 in case of error
 */
int parse_string(char *line, const char *key, char *result) {
  char *value;

  if (strcmp(strtok(line, "="), key) == 0) {

    if ((value = strtok(NULL, "\n")) == NULL) {
      warn("strtok");
      return -1;
    }

    if (strlen(value) >= strlen(result) - 1) {
      warnx("%s value too long", key);
      return -1;
    }

    strcpy(result, value);

    return 0;
  }

  return -1;
}

/**
 * @brief parses the key value delimited by '=', as long
 *
 * @param line the string to parse
 * @param key the key to search for
 * @param result the value of the key
 *
 * @returns 0 if everything went well or -1 in case of error
 */
int parse_long(char *line, const char *key, long *result) {
  char *value;
  char *notconv;

  if (strcmp(strtok(line, "="), key) == 0) {

    if ((value = strtok(NULL, "\n")) == NULL) {
      warn("strtok");
      return -1;
    }

    *result = strtol(value, &notconv, 10);
    if (errno != 0 || *notconv != '\0') {
      warn("strtol");
      return -1;
    }

    return 0;
  }

  return -1;
}
