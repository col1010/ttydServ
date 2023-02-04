#include<stdint.h>

#pragma pack(push, 1)

#define INITIAL_HEALTH 1000

#define MESSAGE_HEADER_SIZE 67
struct message {
    uint8_t type; // 1
    uint16_t msg_len;
    char recipient[32];
    char sender[32];
    char* message;
}__attribute__((packed));

#define ERROR_HEADER_SIZE 4
struct error {
    uint8_t type; // 7
    uint8_t err_code;
    uint16_t msg_len;
    char message[1024];
}__attribute__((packed));

#define ACCEPT_HEADER_SIZE 2
struct accept {
    uint8_t type; // 8
    uint8_t accept_type;
}__attribute__((packed));

#define ROOM_HEADER_SIZE 37
struct room {
    uint8_t type; // 9, change to 10 for a connection type
    uint16_t room_num;
    char room_name[32];
    uint16_t desc_len;
    char* description;
    uint8_t locked; // not park of lurk, but allows for server-side checking of locked doors, denied access, etc
}__attribute__((packed));

// define the player flags
#define ALIVE 0b10000000
#define JOIN_BATTLE 0b01000000
#define MONSTER 0b00100000
#define STARTED 0b00010000
#define READY 0b00001000

#define CHARACTER_HEADER_SIZE 48
struct character {
    uint8_t type; // 10
    char name[32];
    uint8_t flags;
    uint16_t attack;
    uint16_t defense;
    uint16_t regen;
    int16_t health;
    uint16_t gold;
    uint16_t room_num;
    uint16_t desc_len;
    char* description;
    int16_t fd; // not part of lurk, but helpful server-side
    uint8_t npc; // also not part of lurk, but used to differentiate between people-made characters and NPCs
    int16_t initial_health; // also not part of lurk, used to track the initial health of enemies
}__attribute__((packed));

#define STAT_LIMIT 65535
#define INITIAL_STATS 1000

#define GAME_HEADER_SIZE 7
struct game {
    uint8_t type; // 11
    uint16_t init_points;
    uint16_t stat_limit;
    uint16_t desc_len;
    char description[1500];
}__attribute__((packed));

/* not needed, just send room struct with the type changed to 13
#define CONNECTION_HEADER_SIZE 37
struct connection {
    const uint8_t type = 13;
    uint16_t room_num;
    char room_name[32];
    uint16_t desc_len;
    char* description;
}__attribute__((packed));
*/

#define VERSION_HEADER_SIZE 5
struct version {
    uint8_t type; // 14
    uint8_t major; // 2
    uint8_t minor; // 3
    uint16_t exten_size; // 0
    // list of extensions not implemented
}__attribute__((packed));

#pragma pack(pop)