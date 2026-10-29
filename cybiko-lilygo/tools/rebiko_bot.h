#ifndef REBIKO_BOT_H
#define REBIKO_BOT_H

/*
 * ReBiko Desktop's chat bot (the Classic only): a second, hidden Cybiko
 * Classic, a clone of the Desktop's with its own CyID and nickname, run by
 * a local LLM (Ollama). It opens Chat, walks into a room, and answers every
 * line said there by typing into CyOS's Chat, so CyOS sends the packets as
 * it would for a person.
 *
 * The air: inside the app, the Desktop's Cybiko, the bot's and the Heltec
 * link (when there is one) share one radio. Each port's frames reach the
 * other port and the link; the link's reach both. So the bot is heard by
 * the Desktop's own Cybiko and by T-Decks, with one Heltec or none. The bot
 * reads Chat lines from the frames that reach it (type 0x28, the text in
 * the clear after 0x25 and a length).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/emulator.h"
#include "radio_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the in-app air ---- */

#define RB_AIR_PORTS 2

typedef struct rb_air rb_air_t;

typedef struct {
    cybiko_radio_transport_t base;
    rb_air_t *air;
    int idx;
    cybiko_radio_on_packet_fn fn;
    void *fn_user;
} rb_air_port_t;

struct rb_air {
    rb_air_port_t port[RB_AIR_PORTS];
    cybiko_radio_transport_t *link;     /* the Heltec, or NULL */
    /* every frame that reaches a port, for the bot's ears */
    void (*heard)(void *user, int port, const uint8_t *data, size_t len, uint8_t channel);
    void *heard_user;
};

void rb_air_init(rb_air_t *a, uint32_t id0, uint32_t id1);
cybiko_radio_transport_t *rb_air_port(rb_air_t *a, int i);
/* The Heltec link: its frames reach every port, and theirs go out on it */
void rb_air_set_link(rb_air_t *a, cybiko_radio_transport_t *link);

/* ---- the local LLM (Ollama's HTTP API) ---- */

#define RB_LLM_MAX_MODELS 32

typedef struct {
    char server[128];
    char names[RB_LLM_MAX_MODELS][64];
    int n;
    char err[160];
    bool done;          /* set by the worker; read once done */
    void *thread;
} rb_llm_list_t;

/* Fetch the model list on a worker thread; poll ->done */
void rb_llm_list_start(rb_llm_list_t *job, const char *server);
/* Once ->done: joins the worker */
void rb_llm_list_finish(rb_llm_list_t *job);

/* ---- the bot ---- */

typedef struct {
    const char *name;   /* as CyOS's Chat lists it */
    uint8_t channel;
} rb_room_t;

#define RB_ROOMS 6
extern const rb_room_t RB_ROOM[RB_ROOMS];   /* in the Chat menu's order */

typedef struct rb_bot rb_bot_t;

#define RB_BOT_NICK "Tyler"   /* a friend from 2000, goofy (rebiko_bot.c's prompt) */

/* Set the nickname in a Classic flash image (CyOS's res-finder.dat) */
bool rb_bot_set_nick(uint8_t *flash, size_t len, const char *nick);
/* Boot the bot's own Cybiko from flash on port 1 of the air; once CyOS is
 * at its desktop it opens Chat, walks into the room, and answers what is
 * said there. Its flash is saved to save_path when it stops. */
rb_bot_t *rb_bot_start(const uint8_t *boot, size_t boot_n, const uint8_t *flash, size_t flash_n,
                       const char *save_path, uint32_t cyid, rb_air_t *air, const char *nick,
                       const char *server, const char *model, int room);
/* Once a Desktop frame */
void rb_bot_frame(rb_bot_t *b);
/* Where the bot's Cybiko draws its screen (each frame it changes) */
void rb_bot_set_view(rb_bot_t *b, void (*fn)(void *ctx, const uint8_t *px, int w, int h),
                     void *ctx);
/* The person's key matrix, pressed on the bot's Cybiko too; NULL: none */
void rb_bot_set_user_keys(rb_bot_t *b, const uint16_t *keys);
void rb_bot_stop(rb_bot_t *b);
/* One line for the LLM window; changes when something happens */
const char *rb_bot_status(const rb_bot_t *b);
/* The last line heard and said, for the window */
const char *rb_bot_heard(const rb_bot_t *b);
const char *rb_bot_said(const rb_bot_t *b);

/* For tests: the Chat line in a frame (address header kept), or false */
bool rb_chat_line(const uint8_t *frame, size_t len, uint8_t *channel, uint32_t *from,
                  char *text, size_t cap);
/* For tests: an LLM reply cut to what the Classic can type, up to max */
void rb_bot_clean(const char *in, char *out, size_t max);
#define RB_LINE 60          /* what one Chat line holds */
#define RB_LOOKUP_LINES 3   /* lines for an answer looked up on Wikipedia */
/* A reply cleaned and broken at spaces into up to max_lines Chat lines */
int rb_bot_split(const char *in, char lines[][RB_LINE + 1], int max_lines);

#ifdef __cplusplus
}
#endif

#endif
