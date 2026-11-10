#ifndef MYVAVA_SERVER_WEB_H
#define MYVAVA_SERVER_WEB_H
struct server_web;
struct file_store;
struct mvm_store;
struct server_web_config {
    const char *state,*socket,*bind,*origin,*directory,*bootstrap,*certificate,*key,*ffmpeg;
    unsigned port;
    struct file_store *files;
    struct mvm_store *media;
};
struct server_web *server_web_start(const struct server_web_config *);
void server_web_stop(struct server_web *);
#endif
