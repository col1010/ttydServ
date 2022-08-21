#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "lurk_structs.h"
#include <stdio.h>

character* receive_character(int fd) {
    character *c = (character*) malloc(CHARACTER_HEADER_SIZE + sizeof(size_t) + sizeof(int16_t) + sizeof(uint8_t)); // add a size_t size for the char*, add int16_t size for the client_fd at the end, add uint8_t size for npc bool
    size_t readlen = recv(fd, &c->name, CHARACTER_HEADER_SIZE - 1, MSG_WAITALL);
    c->type = 10;
    c->name[31] = 0; // ensure the name sent in is null terminated
    c->description = (char*) malloc(c->desc_len + 1);
    c->description[c->desc_len] = 0; // null terminate the description
    c->npc = 0; // players are not NPCs
    printf("Beginning to receive character from fd %d\n", fd);
    if (c->desc_len != 0)
        recv(fd, c->description, c->desc_len, MSG_WAITALL);
    printf("Done!\n");
    return c;
}

uint16_t receive_changeroom(int fd) {
    uint16_t room_num;
    recv(fd, &room_num, 2, MSG_WAITALL); // receive the room number
    return room_num;
}

message* receive_message(int fd) {
    message *m = (message*) malloc(MESSAGE_HEADER_SIZE + sizeof(size_t)); // add a size_t for the char*
    recv(fd, &m->msg_len, MESSAGE_HEADER_SIZE - 1, MSG_WAITALL); // read everything but the actual message
    m->type = 1;
    m->sender[31] = 0;
    m->recipient[31] = 0;
    m->message = (char*) malloc(m->msg_len + 1);
    m->message[m->msg_len] = 0; // null terminate the message
    if (m->msg_len != 0)
        recv(fd, m->message, m->msg_len, MSG_WAITALL);
    return m;
}