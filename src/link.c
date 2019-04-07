#include "link.h"

#include "network.h"

#include <gumbo.h>

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define HTTP_OK 200
#define HTTP_PARTIAL_CONTENT 206
#define HTTP_RANGE_NOT_SATISFIABLE 416

/* ---------------- External variables -----------------------*/
LinkTable *ROOT_LINK_TBL = NULL;

static void HTML_to_LinkTable(GumboNode *node, LinkTable *linktbl);
static Link *Link_new(const char *p_url, LinkType type);
static CURL *Link_to_curl(Link *link);
static void Link_get_stat(Link *this_link);
static void LinkTable_add(LinkTable *linktbl, Link *link);
void LinkTable_fill(LinkTable *linktbl);
static void LinkTable_free(LinkTable *linktbl);
static void LinkTable_print(LinkTable *linktbl);
static Link *path_to_Link_recursive(char *path, LinkTable *linktbl);
static LinkType p_url_type(const char *p_url);
static char *url_append(const char *url, const char *sublink);

/**
 * Shamelessly copied and pasted from:
 * https://github.com/google/gumbo-parser/blob/master/examples/find_links.cc
 */
static void HTML_to_LinkTable(GumboNode *node, LinkTable *linktbl)
{
    if (node->type != GUMBO_NODE_ELEMENT) {
        return;
    }
    GumboAttribute* href;

    if (node->v.element.tag == GUMBO_TAG_A &&
        (href = gumbo_get_attribute(&node->v.element.attributes, "href"))) {
        /* if it is valid, copy the link onto the heap */
        LinkType type = p_url_type(href->value);
    if (type) {
        LinkTable_add(linktbl, Link_new(href->value, type));
    }
        }
        /* Note the recursive call, lol. */
        GumboVector *children = &node->v.element.children;
        for (size_t i = 0; i < children->length; ++i) {
            HTML_to_LinkTable((GumboNode*)children->data[i], linktbl);
        }
        return;
}

static Link *Link_new(const char *p_url, LinkType type)
{
    Link *link = calloc(1, sizeof(Link));
    if (!link) {
        fprintf(stderr, "Link_new(): calloc failure!\n");
        exit(EXIT_FAILURE);
    }
    strncpy(link->p_url, p_url, LINK_LEN_MAX);
    link->type = type;

    /* remove the '/' from p_url if it exists */
    char *c = &(link->p_url[strnlen(link->p_url, LINK_LEN_MAX) - 1]);
    if ( *c == '/') {
        *c = '\0';
    }

    return link;
}

static CURL *Link_to_curl(Link *link)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Link_to_curl(): curl_easy_init() failed!\n");
    }
    /* set up some basic curl stuff */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "HTTPDirFS");
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    /* for following directories without the '/' */
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 2);
    curl_easy_setopt(curl, CURLOPT_URL, link->f_url);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15);
    curl_easy_setopt(curl, CURLOPT_SHARE, CURL_SHARE);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);

    if (NETWORK_CONFIG.username) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, NETWORK_CONFIG.username);
    }

    if (NETWORK_CONFIG.password) {
        curl_easy_setopt(curl, CURLOPT_PASSWORD, NETWORK_CONFIG.password);
    }

    if (NETWORK_CONFIG.proxy) {
        curl_easy_setopt(curl, CURLOPT_PROXY, NETWORK_CONFIG.proxy);
    }

    if (NETWORK_CONFIG.proxy_user) {
        curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME,
                         NETWORK_CONFIG.proxy_user);
    }

    if (NETWORK_CONFIG.proxy_pass) {
        curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD,
                         NETWORK_CONFIG.proxy_pass);
    }

    return curl;
}

void Link_get_stat(Link *this_link)
{
    fprintf(stderr, "Link_get_size(%s);\n", this_link->f_url);

    if (this_link->type == LINK_FILE) {
        CURL *curl = Link_to_curl(this_link);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
        curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);

        /*
         * We need to put the variable on the heap, because otherwise the
         * variable gets popped from the stack as the function returns.
         *
         * It gets freed in curl_multi_perform_once();
         */
        TransferStruct *transfer = malloc(sizeof(TransferStruct));
        if (!transfer) {
            fprintf(stderr, "Link_get_size(): malloc failed!\n");
        }
        transfer->link = this_link;
        transfer->type = FILESTAT;
        curl_easy_setopt(curl, CURLOPT_PRIVATE, transfer);

        transfer_nonblocking(curl);
    }
}

void Link_set_stat(Link* this_link, CURL *curl)
{
    long http_resp;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_resp);
    if (http_resp == HTTP_OK) {
        double cl = 0;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl);
        curl_easy_getinfo(curl, CURLINFO_FILETIME, &(this_link->time));

        if (cl == -1) {
            /* Turns out not to be a file after all */
            this_link->content_length = 0;
            this_link->type = LINK_DIR;
        } else {
            this_link->content_length = cl;
            this_link->type = LINK_FILE;
        }
    } else {
        this_link->type = LINK_INVALID;
    }
}

static void LinkTable_add(LinkTable *linktbl, Link *link)
{
    linktbl->num++;
    linktbl->links = realloc(linktbl->links, linktbl->num * sizeof(Link *));
    if (!linktbl->links) {
        fprintf(stderr, "LinkTable_add(): realloc failure!\n");
        exit(EXIT_FAILURE);
    }
    linktbl->links[linktbl->num - 1] = link;
}

void LinkTable_fill(LinkTable *linktbl)
{
    Link *head_link = linktbl->links[0];
    for (int i = 0; i < linktbl->num; i++) {
        Link *this_link = linktbl->links[i];
        if (this_link->type) {
            char *url;
            url = url_append(head_link->f_url, this_link->p_url);
            strncpy(this_link->f_url, url, URL_LEN_MAX);
            free(url);

            char *unescaped_p_url;
            unescaped_p_url = curl_easy_unescape(NULL, this_link->p_url, 0,
                                                 NULL);
            strncpy(this_link->p_url, unescaped_p_url, LINK_LEN_MAX);
            curl_free(unescaped_p_url);

            if (this_link->type == LINK_FILE && !(this_link->content_length)) {
                Link_get_stat(this_link);
            } else if (this_link->type == LINK_DIR) {
                this_link->time = head_link->time;
            }
        }
    }
    /* Block until the LinkTable is filled up */
    while (curl_multi_perform_once()) {
        usleep(1000);
    }
}

static void LinkTable_free(LinkTable *linktbl)
{
    for (int i = 0; i < linktbl->num; i++) {
        free(linktbl->links[i]);
    }
    free(linktbl->links);
    free(linktbl);
}

LinkTable *LinkTable_new(const char *url)
{
    fprintf(stderr, "LinkTable_new(%s);\n", url);

    LinkTable *linktbl = calloc(1, sizeof(LinkTable));
    if (!linktbl) {
        fprintf(stderr, "LinkTable_new(): calloc failure!\n");
        exit(EXIT_FAILURE);
    }

    /* populate the base URL */
    LinkTable_add(linktbl, Link_new("/", LINK_HEAD));
    Link *head_link = linktbl->links[0];
    head_link->type = LINK_HEAD;
    strncpy(head_link->f_url, url, URL_LEN_MAX);

    /* start downloading the base URL */
    CURL *curl = Link_to_curl(head_link);
    MemoryStruct buf;
    buf.size = 0;
    buf.memory = NULL;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buf);

    transfer_blocking(curl);

    /* if downloading base URL failed */
    long http_resp;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_resp);
    if (http_resp != HTTP_OK) {
        fprintf(stderr, "link.c: LinkTable_new() cannot retrieve the base URL, \
URL: %s, HTTP %ld\n", url, http_resp);

        LinkTable_free(linktbl);
        linktbl = NULL;
        return linktbl;
    };
    curl_easy_getinfo(curl, CURLINFO_FILETIME, &(head_link->time));
    curl_easy_cleanup(curl);

    /* Otherwise parsed the received data */
    GumboOutput* output = gumbo_parse(buf.memory);
    HTML_to_LinkTable(output->root, linktbl);
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    free(buf.memory);

    /* Fill in the link table */
    LinkTable_fill(linktbl);
    return linktbl;
}

/** \brief print a LinkTable */
static void LinkTable_print(LinkTable *linktbl)
{
    fprintf(stderr, "--------------------------------------------\n");
    fprintf(stderr, " LinkTable %p for %s\n", linktbl,
            linktbl->links[0]->f_url);
    fprintf(stderr, "--------------------------------------------\n");
    for (int i = 0; i < linktbl->num; i++) {
        Link *this_link = linktbl->links[i];
        fprintf(stderr, "%d %c %zu %s %s\n",
                i,
                this_link->type,
                this_link->content_length,
                this_link->p_url,
                this_link->f_url
        );

    }
    fprintf(stderr, "--------------------------------------------\n");
}

Link *path_to_Link(const char *path)
{
    char *new_path = strndup(path, URL_LEN_MAX);
    if (!new_path) {
        fprintf(stderr, "path_to_Link(): cannot allocate memory\n");
        exit(EXIT_FAILURE);
    }
    Link *link = path_to_Link_recursive(new_path, ROOT_LINK_TBL);
    free(new_path);
    return link;
}

static Link *path_to_Link_recursive(char *path, LinkTable *linktbl)
{
    /* skip the leading '/' if it exists */
    if (*path == '/') {
        path++;
    }

    /* remove the last '/' if it exists */
    char *slash = &(path[strnlen(path, URL_LEN_MAX) - 1]);
    if (*slash == '/') {
        *slash = '\0';
    }

    slash = strchr(path, '/');
    if ( slash == NULL ) {
        /* We cannot find another '/', we have reached the last level */
        for (int i = 1; i < linktbl->num; i++) {
            if (!strncmp(path, linktbl->links[i]->p_url, LINK_LEN_MAX)) {
                /* We found our link */
                return linktbl->links[i];
            }
        }
    } else {
        /*
         * We can still find '/', time to consume the path and traverse
         * the tree structure
         */

        /*
         * add termination mark to  the current string,
         * effective create two substrings
         */
        *slash = '\0';
        /* move the pointer past the '/' */
        char *next_path = slash + 1;
        for (int i = 1; i < linktbl->num; i++) {
            if (!strncmp(path, linktbl->links[i]->p_url, LINK_LEN_MAX)) {
                /* The next sub-directory exists */
                if (!(linktbl->links[i]->next_table)) {
                    linktbl->links[i]->next_table = LinkTable_new(
                        linktbl->links[i]->f_url);
                    fprintf(stderr, "Created new link table for %s\n",
                            linktbl->links[i]->f_url);
                    LinkTable_print(linktbl->links[i]->next_table);
                }

                return path_to_Link_recursive(next_path,
                                              linktbl->links[i]->next_table);
            }
        }
    }
    return NULL;
}

long path_download(const char *path, char *output_buf, size_t size,
                   off_t offset)
{
    Link *link;
    link = path_to_Link(path);
    if (!link) {
        return -ENOENT;
    }

    size_t start = offset;
    size_t end = start + size;
    char range_str[64];
    snprintf(range_str, sizeof(range_str), "%zu-%zu", start, end);

    MemoryStruct buf;
    buf.size = 0;
    buf.memory = NULL;

    fprintf(stderr, "path_download(%s, %s);\n",
            path, range_str);

    CURL *curl = Link_to_curl(link);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buf);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_str);

    transfer_blocking(curl);

    long http_resp;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_resp);
    if ( !(
        (http_resp != HTTP_OK) ||
        (http_resp != HTTP_PARTIAL_CONTENT) ||
        (http_resp != HTTP_RANGE_NOT_SATISFIABLE)
    )) {
        fprintf(stderr, "path_download(): Could not download %s, HTTP %ld\n",
                link->f_url, http_resp);
        return -ENOENT;
    }

    double dl;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &dl);

    size_t recv = dl;
    if (recv > size) {
        recv = size;
    }

    memmove(output_buf, buf.memory, recv);
    curl_easy_cleanup(curl);
    free(buf.memory);
    return recv;
}

static LinkType p_url_type(const char *p_url)
{
    /* The link name has to start with alphanumerical character */
    if (!isalnum(p_url[0])) {
        return LINK_INVALID;
    }

    /* check for http:// and https:// */
    if ( !strncmp(p_url, "http://", 7) || !strncmp(p_url, "https://", 8) ) {
        return LINK_INVALID;
    }

    if ( p_url[strlen(p_url) - 1] == '/' ) {
        return LINK_DIR;
    }

    return LINK_FILE;
}

static char *url_append(const char *url, const char *sublink)
{
    int needs_separator = 0;
    if (url[strlen(url)-1] != '/') {
        needs_separator = 1;
    }

    char *str;
    size_t ul = strlen(url);
    size_t sl = strlen(sublink);
    str = calloc(ul + sl + needs_separator + 1, sizeof(char));
    if (!str) {
        fprintf(stderr, "url_append(): calloc failure!\n");
        exit(EXIT_FAILURE);
    }
    strncpy(str, url, ul);
    if (needs_separator) {
        str[ul] = '/';
    }
    strncat(str, sublink, sl);
    return str;
}
