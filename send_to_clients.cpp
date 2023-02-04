#include "lurk_structs.h"
#include<string.h>
#include<unistd.h>
#include<map>
#include<vector>
#include<sys/types.h>
#include<sys/socket.h>
#include<time.h>
#include<string>
#include<cerrno>
#include<cstdio>

using namespace std;

#define SERVER_NARRATION_NAME "Grodus"

bool send_message(int fd, const char* sender, const char* recipient, const char* message) {
	struct message m;
    m.type = 1;
	m.msg_len = strlen(message) + 1;
	memcpy(m.sender, sender, 32);
	memcpy(m.recipient, recipient, 32);
    char buf[MESSAGE_HEADER_SIZE + m.msg_len];
    memcpy(buf, &m, MESSAGE_HEADER_SIZE);
    strcpy(buf + MESSAGE_HEADER_SIZE, message);
    size_t send_size = MESSAGE_HEADER_SIZE + m.msg_len;
    return send_size == send(fd, buf, send_size, MSG_DONTWAIT);
    /*
    if (send_size != send(fd, buf, send_size, MSG_DONTWAIT)) {
        int err = errno;
        if (err == EAGAIN) {
            printf("Send would have blocked in send_message! Waiting now...\n");
            fd_set set;
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 0; // 100000 microseconds = 100 ms
            FD_ZERO(&set);
            FD_SET(fd, &set);
            while (select(1, NULL, &set, NULL, &timeout) == 0) { // while the socket buffer is full
                // do nothing
                printf("Timeout!\n");
            }
            printf("Past the select\n");
            send(fd, buf, send_size, MSG_DONTWAIT);
        }
    }
    */
}

bool send_accept(int fd, uint8_t acc_type) {
    struct accept a;
    a.type = 8;
    a.accept_type = acc_type;
    return ACCEPT_HEADER_SIZE == write(fd, &a, 2); // send the accept message
}

bool send_game(int fd) {
	struct game g;
    g.type = 11;
    g.stat_limit = STAT_LIMIT;
    g.init_points = INITIAL_STATS;
	strcpy(g.description, "\n                       _  ______                   \n   _     _            | |/ _____)                  \n _| |_ _| |_ _   _  __| ( (____  _____  ____ _   _ \n(_   _|_   _) | | |/ _  |\\____ \\| ___ |/ ___) | | |\n  | |_  | |_| |_| ( (_| |_____) ) ____| |    \\ V / \n   \\__)  \\__)\\__  |\\____(______/|_____)_|     \\_/  \n            (____/                                 \n\nYou find yourself in a sort of crossover world between a few instant classic Nintendo games from the 2000s: Paper Mario: The Thousand Year Door and the Metroid Prime series. You begin in the same place you would in Thousand Year Door, and many of the locations and characters from the original game are here. However, some of the locations have been replaced with a Metroid Prime or Prime 2 location instead. As of now, there are 67 rooms and 103 total NPCs / enemies. A custom wiki with lists of all rooms, NPCs, enemies, and what they reference is available at http://isoptera.lcsc.edu/~cmkauffman/wiki/home.html");
	g.desc_len = strlen(g.description);
	size_t send_size = GAME_HEADER_SIZE + g.desc_len;
	return send_size == send(fd, &g, send_size, MSG_DONTWAIT);
}

bool send_version(int fd) {
	struct version v;
    v.type = 14;
    v.major = 2;
    v.minor = 3;
    v.exten_size = 0;
	return VERSION_HEADER_SIZE == send(fd, &v, 5, MSG_DONTWAIT);
}

bool send_error(int fd, uint8_t err_code, const char* err_msg) {
    struct error e;
    e.type = 7;
    e.err_code = err_code;
    strncpy(e.message, err_msg, 1024);
    e.msg_len = strlen(e.message);
    size_t send_size = ERROR_HEADER_SIZE + e.msg_len;
    return send_size == send(fd, &e, send_size, MSG_DONTWAIT);
}

bool send_room(int fd, room* r) {
    size_t send_size = ROOM_HEADER_SIZE + r->desc_len;
    char buf[ROOM_HEADER_SIZE + r->desc_len]; // create a buffer
    memcpy(buf, r, ROOM_HEADER_SIZE); // copy everything but the description into the buffer
    strcpy(buf + ROOM_HEADER_SIZE, r->description); // copy the description into the buffer
    return send_size == send(fd, buf, send_size, MSG_DONTWAIT);
}

bool send_connections(int fd, map<uint16_t, room>* room_map, vector<uint16_t>* connections) {
    for (auto room_num : *connections) { // for each room number in the connection vector
        room *tmp = &room_map->at(room_num); // make a temp room
        char buf[ROOM_HEADER_SIZE + tmp->desc_len]; // make a buffer to hold the room
        buf[0] = 13; // type 13 for connection
        memcpy(buf + 1, &tmp->room_num, ROOM_HEADER_SIZE - 1); // copy everything but the type and the description into the buffer
        strcpy(buf + ROOM_HEADER_SIZE, tmp->description); // copy the description into the buffer
        size_t send_size = ROOM_HEADER_SIZE + tmp->desc_len; // set the send size
        if (send_size != send(fd, buf, send_size, MSG_DONTWAIT)) { // if the send size does not match the connection sent
            return false;
        }
    }
    return true;
}

// send a character. Lock character_mutex before calling
bool send_character(int fd, character* ch) {
    char buf[CHARACTER_HEADER_SIZE + ch->desc_len]; // create an appropriately sized buffer to send
    memcpy(buf, ch, CHARACTER_HEADER_SIZE); // copy everything but the description
    strncpy(buf + CHARACTER_HEADER_SIZE, ch->description, ch->desc_len); // copy the description and ensure the null terminator is copied correctly
    size_t send_size = CHARACTER_HEADER_SIZE + ch->desc_len;
    return send_size == send(fd, buf, send_size, MSG_DONTWAIT);
}

bool send_msg_to_all(map<string, character*>* character_map, const char * msg) {
    uint8_t successful = true;
    char narrator[32] = SERVER_NARRATION_NAME;
    narrator[30] = 0;
    narrator[31] = 1; // mark the sender as narrator (lurk 2.3)
    for (auto c : *character_map) { // for every player in the server
        if (c.second->fd == -1) continue; // skip if the player is not presently in the server or they're an NPC
        if (!send_message(c.second->fd, narrator, c.second->name, msg)) { // if the message was not sent successfully
            int err = errno;
            //printf("Errno: %d\n", err);
            printf("Failed in send_msg_to_all to send message to %s\n", c.second->name);
            successful = false;
        }
    }
    return successful;
}

bool send_msg_to_all_in_room(uint16_t room_num, map<uint16_t, vector<character*>>* room_characters_map, const char * msg) {
    uint8_t successful = 1;
    char narrator[32] = SERVER_NARRATION_NAME;
    narrator[30] = 0;
    narrator[31] = 1; // mark the sender as narrator (lurk 2.3)
    for (auto c : room_characters_map->at(room_num)) { // for every player in the room
        if (c->fd == -1) continue; // skip if the player is not presently in the server or they're an NPC
        if (!send_message(c->fd, narrator, c->name, msg)) { // if the message was not sent successfully
            printf("Failed in send_msg_to_all_in_toom to send message to %s\n", c->name);
            successful = 0;
        }
    }
    return successful;
}

bool send_narrator_msg(int fd, const char* recipient, const char* message) {
    char narrator[32] = SERVER_NARRATION_NAME;
    narrator[30] = 0;
    narrator[31] = 1; // mark the sender as narrator (lurk 2.3)
    struct message m;
    m.type = 1;
	m.msg_len = strlen(message);
	memcpy(m.sender, narrator, 32);
	memcpy(m.recipient, recipient, 32);
    char buf[MESSAGE_HEADER_SIZE + m.msg_len];
    memcpy(buf, &m, MESSAGE_HEADER_SIZE);
    strcpy(buf + MESSAGE_HEADER_SIZE, message);
    size_t send_size = MESSAGE_HEADER_SIZE + m.msg_len;
    return send_size == send(fd, buf, send_size, MSG_DONTWAIT);
}

// attempts to send every character in a given room to the client
bool send_characters_in_room(int fd, map<uint16_t, vector<character*>>* room_characters_map, uint16_t room_num) {
    uint8_t successful = true;
    if (room_characters_map->empty())
        return successful;
        
    for (auto c : room_characters_map->at(room_num)) { // for every character in the specified room
        if (!send_character(fd, c)) { // attempt to send each character to the client
            int err = errno;
            printf("Errno: %d\n", err);
            printf("send_characters_in_room failed to send character update to %s\n", c->name);
            successful = false;
        }
    }
    return successful;
}

// attempts to send a character to every player in a given room
bool send_ch_to_all_in_room(character* character_to_send, map<uint16_t, vector<character*>>* room_characters_map, uint16_t room_num) {
    uint8_t successful = true;
    if (room_characters_map->empty())
        return successful;
    
    for (auto c : room_characters_map->at(room_num)) { // for every character in the specified room
        if (c->fd == -1) continue; // skip characters that are no longer active or are NPCs / enemies
        if (!send_character(c->fd, character_to_send)) { // attempt to send the character to each client
            int err = errno;
            printf("Errno: %d\n", err);
            printf("send_ch_to_all_in_room failed to send character update to %s\n", c->name);
            successful = false;
        }
    }
    return successful;
}