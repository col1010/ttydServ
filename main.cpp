// build like this: g++ main.cpp send_to_clients.cpp receive_from_clients.cpp calculate_fights.cpp -o server -pthread
// Note: using optimizing options currently break the program

#include "lurk_structs.h"
#include "send_to_clients.h"
#include "receive_from_clients.h"
#include "calculate_fights.h"

#include "rapidjson-1.1.0/include/rapidjson/document.h"
#include "rapidjson-1.1.0/include/rapidjson/istreamwrapper.h"
#include<sys/socket.h>
#include<sys/types.h>
#include<netinet/ip.h>
#include<arpa/inet.h>
#include<stdio.h>
#include<unistd.h>
#include<signal.h>
#include<sys/stat.h>
#include<poll.h>
#include<vector>
#include<map>
#include<set>
#include<random>
#include<cstring>
#include<fstream>
#include<iostream>
#include<cstdlib>
#include<thread>
#include<mutex>
#include<algorithm>
#include<atomic>
#include<chrono>
#include<ctime>

using namespace std;

void close_server(int signal_number);
void handle_client(int client_fd);
void set_up_rooms();
void set_up_NPCs();
void free_character(struct character* c);
void change_room(int fd, uint16_t room_num, uint16_t cur_room, character* c);
void handle_disconnect(character* c);
void handle_pvp(character* player, character* npc);
bool create_bots(uint16_t room_num);
string get_time();
void set_socklim(int sockfd);

const char* LURK_NAMES[] = {"Unused", "Message", "Changeroom", "Fight", "PVP Fight", "Loot", "Start", "Error", "Accept", "Room", "Character", "Game", "Leave", "Connection", "Version"};

int skt; // the socket the server will listen on
map<string, character*> character_map;
mutex character_mutex; // lock when inserting / removing, but also when updating characters because any thread may access any character at any time

map<uint16_t, vector<character*>> room_characters_map; // map containing the characters (including monsters) present in each room, lock character_mutex when editing

map<uint16_t, vector<character*>> room_enemies_map; // map containing just the monsters present in each room for quick accession in fight calculation, lock character_mutex when editing

map<uint16_t, room> room_map; // map populated on server initialization that contains the basic information about each room
map<uint16_t, vector<uint16_t>> connection_map; // map populated on server initialization that contains room numbers and their associated connections
set<string> allowed_in_twilight_town; // a set including the names of players who are allowed into Twilight Town. Players must locate Darkly in-game and send a pvp request to him to gain access to Twilight Town

map<thread::id, thread> threads;
mutex thread_mutex; // lock when inserting / removing threads from the set

atomic<bool> exit_thread {false}; // set an atomic bool to false that controls when threads handling clients should terminate

int main(int argc, char** argv) {
    struct sockaddr_in sad;
	if(argc < 2) {
		printf("Usage: %s port\n", argv[0]);
        exit(EXIT_FAILURE);
    } else
		sad.sin_port = htons(atoi(argv[1]));

	sad.sin_addr.s_addr = INADDR_ANY;
	sad.sin_family = AF_INET;


    printf("Initializing maps from maps.json...\n");
    set_up_rooms(); // initialize the map
    printf("Initializing NPCs from NPCs.json\n");
    set_up_NPCs();

	skt = socket(AF_INET, SOCK_STREAM, 0);
	if(skt == -1){
		perror("socket");
		return 1;
	}

    set_socklim(skt);
    struct sigaction ignore_sigpipe;
    ignore_sigpipe.sa_handler = SIG_IGN; // ignore sigpipes and deal with write errors on a thread-by-thread basis
    sigaction(SIGPIPE, &ignore_sigpipe, 0);

	struct sigaction close_action;
	close_action.sa_handler = close_server;
	if(sigaction(SIGINT, &close_action, 0)){
		perror("sigaction");
		return 1;
	}

	if( bind(skt, (struct sockaddr *)(&sad), sizeof(struct sockaddr_in)) ){
		perror("bind");
        close_server(SIGINT);
		return 1;
	}
	if( listen(skt, 5) ){
		perror("listen");
		return 1;
    }
    printf("%s: Listening on port %d\n", get_time().c_str(), stoi(argv[1]));
	int client_fd;
	struct sockaddr_in client_address;
	socklen_t address_size = sizeof(struct sockaddr_in);
	for(;;){
		client_fd = accept(skt, (struct sockaddr *)(&client_address), &address_size);
		if(client_fd == -1){
			perror("accept");
			break;
		}
        printf("%s: Connection made from address %s\n", get_time().c_str(), inet_ntoa(client_address.sin_addr));
        thread t (handle_client, client_fd); // create a thread to handle the client
        printf("%s: Thread with ID %x started\n", get_time().c_str(), t.get_id());
        thread_mutex.lock();
        threads.insert(make_pair(t.get_id(), move(t)));
        thread_mutex.unlock();
	}
    return 0;
}

void handle_client(int client_fd) {
	send_version(client_fd);
	send_game(client_fd);
    struct character* c; // the client's character
    bool received_character = false, started = false, revived = false;
    uint16_t cur_room; // keep track of the current room in a local variable rather than just referencing the c->room_num to reduce mutex locking
    uint8_t type;
    string client_name = "[not_started_yet]";
    char bot_add_code = 0; // used to activate the bot cheat code which adds 5 bots to the current room
    /* to activate the bot code, the client must send 2 START requests followed by a CHANGEROOM request to room 0 */

    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLIN;
    struct pollfd fd_arr[1];
    fd_arr[0] = pfd;
    
    while (!exit_thread) {

        if (poll(fd_arr, 1, 500) == 0) { // continue the loop and check exit_thread bool every 500 ms that nothing has been sent
            continue;
        }

        if (recv(client_fd, &type, 1, 0) != 1) {
            printf("%s: \"%s\" (fd %d) has disconnected ungracefully\n", get_time().c_str(), client_name.c_str(), client_fd);
            if (received_character) { // if the client created a character before disconnecting, handle it appropriately, else just close the fd and exit the thread
                    character_mutex.lock();
                    handle_disconnect(c);
                    character_mutex.unlock();
                }
            close(client_fd);
            thread_mutex.lock();
            threads.find(this_thread::get_id())->second.detach();
            threads.erase(this_thread::get_id());
            thread_mutex.unlock();
            return;
        }
        if (type > 0 && type < 13)
            printf("\n%s: Type received from \"%s\" (fd %d): %u (%s)\n", get_time().c_str(), client_name.c_str(), client_fd, type, LURK_NAMES[type]);

        if (type != 6 && type != 2)
            bot_add_code = 0; // reset the bot code

        if (type == 1) { // MESSAGE
            message *msg = receive_message(client_fd); // receive the message
            printf("%s: Player \"%s\" wishes to send a message to \"%s\"\n", get_time().c_str(), msg->sender, msg->recipient);
            if (!received_character) {
                send_error(client_fd, 5, "Message cannot be delivered. Make a character and start first.");
                printf("%s: Message delivery failed, player has not created a character yet\n", get_time().c_str());
                free(msg->message);
                free(msg);
                continue;
            } else if (!started) {
                send_error(client_fd, 5, "Message cannot be delivered. Start the game first.");
                printf("%s: Message delivery failed, player has not started the game yet\n", get_time().c_str());
                free(msg->message);
                free(msg);
                continue;
            }
            string tmp_recipient = string(msg->recipient);
            character_mutex.lock();

            if (character_map.find(tmp_recipient) == character_map.end()) { // if the recipient does not exist in the server
                character_mutex.unlock();
                send_error(client_fd, 6, "Player with that name not found.");
                printf("%s: Message delivery failed, recipient not found\n", get_time().c_str());
            } else if (character_map.at(tmp_recipient)->npc) { // if the character is an NPC, disallow sending messages to them
                character_mutex.unlock();
                send_error(client_fd, 6, "The requested character is an NPC. They cannot receive messages.");
                printf("%s: Message delivery failed, recipient is an NPC\n", get_time().c_str());
            } else if (character_map.at(tmp_recipient)->fd == -1) {
                character_mutex.unlock();
                send_error(client_fd, 6, "The requested player is not currently online.");
                printf("%s: Message delivery failed, recipient is not currently online\n", get_time().c_str());
            } else {
                send_message(character_map.at(tmp_recipient)->fd, msg->sender, msg->recipient, msg->message); // send the message
                character_mutex.unlock();
                send_accept(client_fd, 1); // send the message accept to the client
                printf("%s: Message delivery successful\n", get_time().c_str());
            }
            free(msg->message);
            free(msg);

        } else if (type == 2) { // CHANGEROOM
            uint16_t room_num = receive_changeroom(client_fd);
            printf("%s: \"%s\" wishes to move from room %u to room %u\n", get_time().c_str(), client_name.c_str(), cur_room, room_num);
            if (!started) {
                send_error(client_fd, 5, "Not ready! Ensure you have made a character and started the game.");
                printf("%s: Changeroom unsuccessful, player has not started the game yet\n", get_time().c_str());
                continue;
            }
            if (bot_add_code == 2 && room_num == 0) { // if the user just sent two START requests and requests a room change to room 0, activate the bot code
                if (!create_bots(cur_room)) // attempt to create the bots and send them to the client's room
                    send_error(client_fd, 0, "Error! Cannot create bots at this moment");
                bot_add_code = 0;
                continue;
            } else if (room_num != 0) {
                bot_add_code = 0;
            }
            if (room_map.find(room_num) == room_map.end()) {
                send_error(client_fd, 1, "Requested room does not exist");
                printf("%s: Changeroom unsuccessful, requested room %u does not exist\n", get_time().c_str(), room_num);
                continue;
            }

            if (find(connection_map.at(cur_room).begin(), connection_map.at(cur_room).end(), room_num) != connection_map.at(cur_room).end()) { // if the requested room is connected to the current room
                character_mutex.lock();

                if (room_num == 33) // room 33 is the Twilight Town Pipe
                    if (allowed_in_twilight_town.find(string(c->name)) == allowed_in_twilight_town.end()) { // if the player is not in the set, disallow access
                        send_error(client_fd, 1, "Access denied. To visit Twilight Town, you must first talk to (send a pvp request to) Darkly in the hidden alley of East Rogueport.");
                        printf("%s: Changeroom unsuccessful, player does not have access to Twilight Town\n", get_time().c_str());
                        character_mutex.unlock();
                        continue;
                    }
                
                if (room_map.at(room_num).locked) {
                    if (strcmp(c->description, "Auto-generated test player") != 0) { // if the player is not a bot created by lurktest, disallow access
                        character_mutex.unlock();
                        send_error(client_fd, 1, "Access denied. The door is locked.");
                        printf("%s: Changeroom unsuccessful, door is locked\n", get_time().c_str());
                        continue;
                    }
                }
                if (!(c->flags & ALIVE)) { // if the player is dead
                    character_mutex.unlock();
                    send_error(client_fd, 5, "You are dead. You cannot change rooms.");
                    printf("%s: Changeroom unsuccessful, player is deceased\n", get_time().c_str());
                    continue;
                }
                send_accept(client_fd, 2);
                change_room(client_fd, room_num, cur_room, c);
                character_mutex.unlock();
                cur_room = room_num;
            } else {
                send_error(client_fd, 1, "You cannot reach that room from here!");
                printf("%s: Changeroom unsuccessful, room is inaccessible from current location\n", get_time().c_str());
            }
        
        }  else if (type == 3) { // FIGHT
            printf("%s: \"%s\" would like to initiate a fight in room %u\n", get_time().c_str(), client_name.c_str(), cur_room);
            if (!received_character) {
                send_error(client_fd, 5, "Fight cannot be initiated. Make a character and start the game first.");
                printf("%s: Fight unsuccessful, player has not created a character yet\n", get_time().c_str());
                continue;
            } else if (!started) {
                send_error(client_fd, 5, "Fight cannot be initiated. Start the game first.");
                printf("%s: Fight unsuccessful, player has not started the game yet\n", get_time().c_str());
                continue;
            }
            character_mutex.lock();
            initiate_fight(c, &room_characters_map, &room_enemies_map);
            if (c->health < 300 && c->health > 0) {
                send_narrator_msg(client_fd, c->name, "Your health is running low! Head to the Rogueport Inn (Room 8, connected to Rogueport Plaza via Podley's Place) to recover your health!");
            }
            character_mutex.unlock();
            printf("%s: Fight sequence completed\n", get_time().c_str());
            
        } else if (type == 4) { // PVPFIGHT
            char name_buf[32];
            recv(client_fd, name_buf, 32, MSG_WAITALL); // receive the name
            printf("%s: \"%s\" has submitted a PVP request to \"%s\"\n", get_time().c_str(), client_name.c_str(), name_buf);
            if (!started) {
                send_error(client_fd, 5, "Not ready. Make a character and start the game first.");
                printf("%s: PVP unsuccessful, player has not started the game yet\n", get_time().c_str());
                continue;
            }
            string tmp_name = string(name_buf);
            character_mutex.lock();
            if (!(c->flags & ALIVE)) {
                character_mutex.unlock();
                send_error(client_fd, 5, "PVP failed. You are dead.");
                printf("%s: PVP unsuccessful, player is deceased\n", get_time().c_str());
                continue;
            }
            if (character_map.find(tmp_name) == character_map.end()) { // if the character is nonexistent
                character_mutex.unlock();
                send_error(client_fd, 0, "PVP failed. Player nonexistent.");
                printf("%s: PVP unsuccessful, requested player does not exist\n", get_time().c_str());
                continue;
            }
            character* tmp_ch = character_map.at(tmp_name);
            if (tmp_ch->room_num != cur_room) { // if the character is not in the current room
                character_mutex.unlock();
                send_error(client_fd, 0, "PVP failed. Player not in this room.");
                printf("%s: PVP unsuccessful, player is not in the current room\n", get_time().c_str());
                continue;
            }
            handle_pvp(c, tmp_ch);
            character_mutex.unlock();
            
        } else if (type == 5) { // LOOT
            char name_buf[32];
            recv(client_fd, name_buf, 32, MSG_WAITALL); // receive the name
            printf("%s: \"%s\" would like to loot \"%s\"\n", get_time().c_str(), client_name.c_str(), name_buf);
            if (!started) {
                send_error(client_fd, 5, "Not ready. Make a character and start the game first.");
                printf("%s: Loot unsuccessful, player has not started the game yet\n");
                continue;
            }
            string tmp_name = string(name_buf);
            character_mutex.lock();
            if (!(c->flags & ALIVE)) {
                character_mutex.unlock();
                send_error(client_fd, 5, "Loot failed. You are dead.");
                printf("%s: Loot unsuccessful, player is deceased\n");
                continue;
            }
            if (character_map.find(tmp_name) == character_map.end()) { // if the character is nonexistent
                character_mutex.unlock();
                send_error(client_fd, 3, "Loot failed. Player or monster not found.");
                printf("%s: Loot unsuccessful, requested player does not exist\n");
                continue;
            }
            character* tmp_ch = character_map.at(tmp_name);
            if (tmp_ch->room_num != cur_room) { // if the character is not in the current room
                character_mutex.unlock();
                send_error(client_fd, 3, "Loot failed. Player or monster not in the current room.");
                printf("%s: Loot unsuccessful, requested player is not in the current room\n");
                continue;
            }
            if (tmp_ch->gold == 0) {
                character_mutex.unlock();
                send_error(client_fd, 3, "Loot failed. Player or monster has no gold.");
                printf("%s: Loot unsuccessful, requested player has no gold\n");
                continue;
            }
            if (tmp_ch->flags & ALIVE) {
                character_mutex.unlock();
                send_error(client_fd, 3, "Loot failed. Player or monster is currently alive.");
                printf("%s: Loot unsuccessful, requested player is alive\n");
                continue;
            }
            uint16_t tmp_gold = tmp_ch->gold;
            c->gold += tmp_gold;
            tmp_ch->gold = 0; // remove the looted character's gold
            send_ch_to_all_in_room(tmp_ch, &room_characters_map, cur_room); // send an updated looted character to everyone in the room
            send_ch_to_all_in_room(c, &room_characters_map, cur_room); // send an updated client's character to everyone in the room
            send_narrator_msg(client_fd, c->name, (string("Loot successful! ") + to_string(tmp_gold) + string(" gold was retrieved.")).c_str());
            character_mutex.unlock();
            printf("%s: Loot successful! \"%s\" retrieved %u gold from \"%s\"\n", get_time().c_str(), client_name.c_str(), tmp_gold, name_buf);
            
        } else if (type == 6) { // START
            printf("%s: Player with fd %d would like to start the game\n", get_time().c_str(), client_fd);
            if (!received_character) {
                send_error(client_fd, 0, "Make a character before starting the game");
                printf("%s: Start unsuccessful, player has not yet sent a valid character\n", get_time().c_str());
                continue;
            } else if (started) {
                character_mutex.lock();
                if (!(c->flags & ALIVE)) { // if the player is dead, revive them and place them in the inn
                    c->health = INITIAL_HEALTH;
                    c->flags |= ALIVE; // revive the player
                    change_room(client_fd, 8, c->room_num, c); // change the player's room to the inn
                    send_narrator_msg(client_fd, c->name, "You blacked out and have woken up at the Rogueport Inn. Your health has been restored.");
                    cur_room = c->room_num; // update the local cur_room
                    character_mutex.unlock();
                    printf("%s: \"%s\" has been brought back to life and sent to the inn\n", get_time().c_str(), client_name.c_str());
                } else {
                    character_mutex.unlock();
                    send_error(client_fd, 0, "Game already started");
                    printf("%s: Start unsuccessful, player has already started the game\n", get_time().c_str());
                    if (bot_add_code == 2) // if the user sends START for a third time, reset the bot code
                        bot_add_code = 0;
                    else
                        bot_add_code++; // increase the bot code
                }
                
                continue;
            }
            send_room(client_fd, &room_map.at(cur_room)); // send the current room
            send_connections(client_fd, &room_map, &connection_map.at(cur_room)); // send the connections
            character_mutex.lock();
            c->flags |= STARTED;
            send_character(client_fd, c); // send an updated version of their character to the client
            if (revived) { // if the player was revived, send a different message to everyone and also a message specifically to the client who revived them
                send_msg_to_all(&character_map, (string(c->name) + string(" has been revived!")).c_str());
                send_narrator_msg(client_fd, c->name, (string("Your attack, defense, and regeneration are set to what this character originally had: ") + to_string(c->attack) + string(" ATK, ") + to_string(c->defense) + string(" DEF, and ") + to_string(c->regen) + " REG.").c_str());
                printf("%s: \"%s\" has been revived and will begin in room %u\n", get_time().c_str(), client_name.c_str(), cur_room);
            }
            else {
                send_msg_to_all(&character_map, (string(c->name) + string(" just joined the game!")).c_str());
                printf("%s: \"%s\" has successfully started the game!\n", get_time().c_str(), client_name.c_str());
            }
            send_characters_in_room(client_fd, &room_characters_map, c->room_num);
            send_ch_to_all_in_room(c, &room_characters_map, c->room_num); // send the new character to everyone in the room
            room_characters_map.at(c->room_num).push_back(c); // add the character to the appropriate room
            character_mutex.unlock();
            started = true;

        } else if (type == 10) { // CHARACTER
            if (received_character) { // if the client has already sent a character
                character* tmp = receive_character(client_fd); // receive the character, but get rid of it
                free_character(tmp); // free the character
                send_error(client_fd, 0, "Character has already been created");
                printf("%s: \"%s\" attempted to send another character\n", get_time().c_str(), client_name.c_str());
                continue;
            }

            c = receive_character(client_fd);
            
            string name = string(c->name);
            character_mutex.lock();
            if (c->attack + c->defense + c->regen > INITIAL_STATS) {
                character_mutex.unlock();
                send_error(client_fd, 4, "Inappropriate player stats (too high). Try again");
                free_character(c);
                continue;
            }
            if (character_map.find(name) != character_map.end()) { // if a player with that name is already in the server
                character* tmp = character_map.at(name);
                if (!(tmp->flags & ALIVE) && tmp->fd == -1) { // if the character is dead and their fd has been set to -1, allow them to be revived
                    if (tmp->health <= 0) { // if the character's health is 0 or less, return them to full HP
                        tmp->health = INITIAL_HEALTH;
                    }
                    tmp->flags |= ALIVE | READY | STARTED; // revive the character
                    printf("%s: Character with name \"%s\" was revived by the player with fd %d\n", get_time().c_str(), tmp->name, client_fd);
                    free_character(c); // free the character sent from the user
                    c = tmp; // set the client's character to the one already in the map
                    cur_room = c->room_num;
                    c->fd = client_fd; // update the file descriptor
                    character_mutex.unlock();
                    send_accept(client_fd, 10); // send an accept of type 10 (character)
                    received_character = true;
                    revived = true;
                    client_name = string(c->name);

                } else { // else the player is alive, so do not permit joining
                    send_error(client_fd, 2, "A player with that name is currently playing, choose a different name");
                    printf("%s: Character with name \"%s\" rejected, player with the same name is alive in the server\n", get_time().c_str(), tmp->name);
                    free_character(c);
                    character_mutex.unlock();
                }
                continue;

            } else {

                c->health = INITIAL_HEALTH; // set the player's health
                c->gold = 100; // set the player's gold
                c->room_num = 1; // set the player's room number to the default
                c->npc = 0; // set the player's npc value to false (0)
                cur_room = c->room_num;
                /**
                 * Check for JOIN_BATTLE flag, and set flags accordingly
                 * This ensures that whatever flags the user sent are ignored, save for the JOIN_BATTLE flag
                 */
                if ((c->flags & JOIN_BATTLE) == JOIN_BATTLE) {
                    c->flags = JOIN_BATTLE | READY | ALIVE;
                } else {
                    c->flags = READY | ALIVE;
                }
                client_name = string(c->name);
                c->fd = client_fd; // set the file descriptor
                character_map.insert(make_pair(client_name, c)); // add the character to the map

                if (strcmp(c->description, "Auto-generated test player") == 0) // if a bot is created from lurktest, it will have this description. Allow it access to twilight town, as the test will break without it
                    allowed_in_twilight_town.insert(client_name);
                
                printf("%s: Character created: Name: %s, Attack: %u, Defense: %u, Regen: %u, Description: %s\n", get_time().c_str(), c->name, c->attack, c->defense, c->regen, c->description);
                send_accept(client_fd, 10); // send an accept of type 10 (character)
                send_character(client_fd, c);
                character_mutex.unlock();
                received_character = true;
            }
        
        } else if (type == 12) { // LEAVE
            printf("%s: \"%s\" has sent a leave request\n", get_time().c_str(), client_name.c_str());
            if (received_character) {
                character_mutex.lock();
                handle_disconnect(c);
                character_mutex.unlock();
            }
            close(client_fd);
            thread_mutex.lock();
            threads.find(this_thread::get_id())->second.detach();
            threads.erase(this_thread::get_id());
            thread_mutex.unlock();
            return; // exit the thread
        } else { // if the client sent an out-of-protocol type
            printf("%s: \"%s\" (fd %d) has sent an unsupported type (%u). Disconnecting them now\n", get_time().c_str(), client_name.c_str(), client_fd, type);
            send_error(client_fd, 0, (string("Type " + to_string(type) + string(" is not accepted. Terminating the connection.")).c_str()));
            if (received_character) {
                character_mutex.lock();
                handle_disconnect(c);
                character_mutex.unlock();
            }
            close(client_fd);
            thread_mutex.lock();
            threads.find(this_thread::get_id())->second.detach();
            threads.erase(this_thread::get_id());
            thread_mutex.unlock();
            return;
        }
    }
    printf("%s: Exiting thread %x for player \"%s\" (fd %d)\n", get_time().c_str(), this_thread::get_id(), client_name.c_str(), client_fd);
    character_mutex.lock();
    c->fd = -1; // this ensures other threads do not try to send this client information as the thread is closing
    character_mutex.unlock();
    close(client_fd);
}

void set_up_rooms() {
    struct stat s;
    if (stat("map.json", &s) != 0) {
        printf("map.json not found. Ensure it is present in this directory.\n");
        exit(EXIT_FAILURE);
    }
    ifstream ifs;
    ifs.open("map.json");
    rapidjson::IStreamWrapper isw { ifs };
    rapidjson::Document doc;
    doc.ParseStream(isw);
    ifs.close();
    int num_rooms = 0;
    room tmp_room;
    tmp_room.type = 9;
    for (rapidjson::Value::MemberIterator itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr) {
        strncpy(tmp_room.room_name, itr->value["name"].GetString(), 32); // get the room name
        tmp_room.room_num = stoi(itr->name.GetString()); // set the room number
        tmp_room.desc_len = strlen(itr->value["description"].GetString()); // set the description length
        tmp_room.description = (char*) malloc(tmp_room.desc_len + 1); // allocate the appropriate size
        strcpy(tmp_room.description, itr->value["description"].GetString()); // set the description of the room
        tmp_room.locked = itr->value["locked"].GetInt(); // set the locked value
        room_map.insert(make_pair((uint16_t)tmp_room.room_num, tmp_room)); // insert the room structure into the room_map
        room_characters_map.insert(make_pair((uint16_t)tmp_room.room_num, vector<character*>())); // insert an empty vector into the room character map
        room_enemies_map.insert(make_pair((uint16_t)tmp_room.room_num, vector<character*>())); // insert an empty vector into the room enemies map as well
        vector<uint16_t> tmp_vec;
        for (auto& c : itr->value["connections"].GetArray()) { // for each connection in the connection list in map.json
            tmp_vec.push_back(c.GetInt()); // add it to the vector
        }
        connection_map.insert(make_pair((uint16_t)tmp_room.room_num, tmp_vec)); // add the vector including the room's connections
        num_rooms++;
        //printf("Room desc: %s\n", tmp_room.description);
    }
    printf("Successfully added %d rooms\n\n", num_rooms);
}

void set_up_NPCs() {
    struct stat s;
    if (stat("NPCs.json", &s) != 0) {
        printf("NPCs.json not found. Ensure it is present in this directory.\n");
        exit(EXIT_FAILURE);
    }
    ifstream ifs;
    ifs.open("NPCs.json");
    rapidjson::IStreamWrapper isw { ifs };
    rapidjson::Document doc;
    doc.ParseStream(isw);
    ifs.close();
    int num_NPCs = 0;
    
    for (rapidjson::Value::MemberIterator itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr) {
        character *tmp_char = (character*) malloc(CHARACTER_HEADER_SIZE + sizeof(size_t) + sizeof(int16_t) + sizeof(uint8_t) + sizeof(int16_t)); // create a character
        tmp_char->type = 10;
        strncpy(tmp_char->name, itr->name.GetString(), 32);
        tmp_char->attack = itr->value["attack"].GetInt();
        tmp_char->defense = itr->value["defense"].GetInt();
        tmp_char->regen = itr->value["regen"].GetInt();
        tmp_char->health = itr->value["health"].GetInt();
        tmp_char->initial_health = tmp_char->health;
        tmp_char->gold = itr->value["gold"].GetInt();
        tmp_char->room_num = itr->value["room_num"].GetInt();
        tmp_char->desc_len = strlen(itr->value["description"].GetString()); // set the description length
        tmp_char->description = (char*) malloc(tmp_char->desc_len + 1); // malloc enough space for the description
        strcpy(tmp_char->description, itr->value["description"].GetString()); // copy the description over
        tmp_char->fd = -1; // set the fd to -1 to ensure nothing is attempted to be sent to them
        if (itr->value["monster"].GetInt()) {
            tmp_char->flags = MONSTER | READY | STARTED;
            tmp_char->npc = 0;
            room_enemies_map.at(tmp_char->room_num).push_back(tmp_char); // add the monster to the room enemies map
        } else {
            tmp_char->flags = READY | STARTED;
            tmp_char->npc = 1;
        }
        if (tmp_char->health > 0)
            tmp_char->flags |= ALIVE;
        
        //printf("adding character %s\n", tmp_char->name);
        //printf("desc: %s\n\n", tmp_char->description);
        room_characters_map.at(tmp_char->room_num).push_back(tmp_char); // add the character to the room character map in the appropriate rooms
        character_map.insert(make_pair(string(tmp_char->name), tmp_char));
        num_NPCs++;
    }
    printf("Successfully added %d NPCs\n\n", num_NPCs);
}

void change_room(int fd, uint16_t room_num, uint16_t cur_room, character* c) {
    character_mutex.unlock();
    send_room(fd, &room_map.at(room_num)); // send the new room description
    send_connections(fd, &room_map, &connection_map.at(room_num)); // send the connections
    character_mutex.lock();
    auto it = find(room_characters_map.at(cur_room).begin(), room_characters_map.at(cur_room).end(), c); // get an iterator to this thread's character in the room character map
    room_characters_map.at(cur_room).erase(it); // remove the character from the old room
    c->room_num = room_num; // set the character's room number to the requested room
    send_ch_to_all_in_room(c, &room_characters_map, cur_room); // send an updated character to everyone in the old room
    send_ch_to_all_in_room(c, &room_characters_map, room_num); // send the client's character to everyone in the new room, excluding themselves
    room_characters_map.at(room_num).push_back(c); // add the character to the new room
    send_characters_in_room(fd, &room_characters_map, room_num); // send the list of characters (which includes the client themselves) in the new room to the client
    if (room_num == 32 || room_num == 42)
        send_narrator_msg(fd, c->name, "Playing alone or in a small group? Go ahead and send two START requests followed by a CHANGEROOM request to room 0 to add in 5 bots for assistance.");
}

// disconnect a character from the server, lock character_mutex before calling
void handle_disconnect(character* c) {
    c->flags = (c->flags & (~ALIVE)); // ensure the character is marked as dead
    c->flags = (c->flags & (~READY)); // ensure the character is not ready
    c->flags = (c->flags & (~STARTED)); // ensure the character is not started
    c->fd = -1; // set the file descriptor to -1, which ensures nothing will be sent to it
    send_ch_to_all_in_room(c, &room_characters_map, c->room_num); // send an updated character to everyone in the room
    send_msg_to_all(&character_map, (string(c->name) + string(" has disconnected")).c_str());
}

void handle_pvp(character* player, character* npc) {
    if (strcmp(npc->name, "Merlon") == 0) {
        if (player->gold < 100) {
            send_error(player->fd, 0, "Not enough gold to increase your stats! Merlon requires 100 gold.");
        } else { // increase the player's stats and decrease their gold, then send updated characters
            if (player->attack + player->defense + player->regen + 150 > STAT_LIMIT) {
                send_error(player->fd, 0, "Whoa! Stat limit reached. You cannot increase your stats further!");
                return;
            }
            player->attack += 50;
            player->defense += 50;
            player->regen += 50;
            player->gold -= 100;
            npc->gold += 100; // increase merlon's gold
            // send updated characters
            send_ch_to_all_in_room(player, &room_characters_map, player->room_num);
            send_ch_to_all_in_room(npc, &room_characters_map, npc->room_num);
            send_narrator_msg(player->fd, player->name, "Your stats have been increased by 50 each!");
            printf("%s: \"%s\" has increased their stats by 50 each\n", get_time().c_str(), player->name);
        }
    } else if (strcmp(npc->name, "Innkeeper") == 0) {
        if (player->health >= INITIAL_HEALTH) {
            send_error(player->fd, 0, "You are already at max health or above!");
            return;
        }
        uint16_t heal_price = (INITIAL_HEALTH - player->health) / 2; // calculate the price of healing
        if (player->gold < heal_price) {
            send_error(player->fd, 0, (string("You do not have enough gold to heal. You need ") + to_string(heal_price) + string(" gold to fully heal.")).c_str());
            return;
        }
        player->health = INITIAL_HEALTH; // fully heal the player
        player->gold -= heal_price;
        npc->gold += heal_price;
        // send updated characters
        send_ch_to_all_in_room(player, &room_characters_map, player->room_num);
        send_ch_to_all_in_room(npc, &room_characters_map, npc->room_num);
        send_narrator_msg(player->fd, player->name, "You have been fully healed!");

    } else if (strcmp(npc->name, "Darkly") == 0) {
        if (allowed_in_twilight_town.find(string(player->name)) == allowed_in_twilight_town.end()) { // if the player is not already allowed into the Twilight Town Pipe
            allowed_in_twilight_town.insert(string(player->name)); // add them to the set
            send_narrator_msg(player->fd, player->name, "Darkly wrote your name on your clothes! The Twilight Town Pipe will no longer reject you.");
        } else
            send_narrator_msg(player->fd, player->name, "Darkly has already written your name on your clothes! You have access to Twilight Town.");
    } else {
        send_error(player->fd, 0, "PVP failed. Target character is not interactable.");
        printf("%s: PVP unsuccessful, target is not interactable\n", get_time().c_str());
    }
}

void free_character(struct character* c) {
	free(c->description);
	free(c);
}

void close_server(int signal_number) {

    printf("\n%s: Closing down the server\n\n", get_time().c_str());
    printf("%s: Waiting for threads to finish...\n", get_time().c_str());
    printf("%s: Number of threads in the threads set: %d\n", get_time().c_str(), threads.size());
    exit_thread = true; // set exit_thread to true so the threads handling clients will terminate
    thread_mutex.lock();
    for (auto& t : threads) { // for each thread
        t.second.join(); // wait until it finishes
    }
    thread_mutex.unlock();

    close(skt); // close the socket 
    printf("\n\n%s: Freeing rooms...\n", get_time().c_str());
    for (auto r : room_map) { // free each room
        free(r.second.description);
    }
    printf("%s: Done\n\n", get_time().c_str());
    printf("%s: Freeing characters...\n", get_time().c_str());
    //printf("Number of characters not including NPCs or monsters: %d\n", character_map.size() - 36);
    for (auto& c : character_map) { // free each character
        free_character(c.second);
    }
    printf("%s: Finished!\n\n", get_time().c_str());
    
    exit(EXIT_SUCCESS); // exit
}

bool create_bots(uint16_t room_num) {
    struct stat s;
    if (stat("/usr/bin/only_names", &s) != 0) {
        printf("/usr/bin/only_names not found. Ensure it is present to use the bot feature\n");
        return false;
    }
    static vector<string> name_list;
    static set<string> used_names;
    static random_device rd;
    static mt19937 gen(rd());
    
    if (used_names.size() + 5 >= name_list.size() - 100) { // if a significant number of names have already been used
        return false;
    }

    if (name_list.empty()) { // populate the list
        string tmp;
        ifstream name_file {"/usr/bin/only_names"};
        while (name_file >> tmp) {
            for (int i = 1; i < tmp.length(); i++)
                tmp[i] = tolower(tmp[i]);
            name_list.push_back(tmp);
        }
        name_file.close();
        printf("%d names added to the list\n", name_list.size());
    }
    static uniform_int_distribution<> name_distrib(0, name_list.size() - 1);
    for (int i = 0; i < 5; i++) { // add 5 bots
        string rand_name = name_list[name_distrib(gen)];
        while (!used_names.insert(rand_name).second) { // while the insertion was unsuccessful (name already used)
            rand_name = name_list[name_distrib(gen)]; // try another name
        }
        character* bot = (character*) malloc(CHARACTER_HEADER_SIZE + sizeof(size_t) + sizeof(int16_t) + sizeof(uint8_t) + sizeof(int16_t)); // create a character
        bot->type = 10;
        strncpy(bot->name, rand_name.c_str(), 32);
        bot->attack = INITIAL_STATS / 2;
        bot->defense = INITIAL_STATS / 4;
        bot->regen = INITIAL_STATS / 4;
        bot->flags = ALIVE | READY | STARTED | JOIN_BATTLE;
        bot->health = INITIAL_HEALTH;
        bot->initial_health = INITIAL_HEALTH;
        bot->gold = 100;
        bot->room_num = room_num;
        bot->desc_len = 4; // set the description length
        bot->description = (char*) malloc(4); // malloc enough space for the description
        strncpy(bot->description, "Bot", 4);
        bot->fd = -1; // set the fd to -1 to ensure nothing is attempted to be sent to them
        bot->npc = 0;
        character_mutex.lock();
        character_map.insert(make_pair(rand_name, bot)); // add the bot to the server
        room_characters_map.at(room_num).push_back(bot); // add the bot to the appropriate room
        send_ch_to_all_in_room(bot, &room_characters_map, room_num); // send the bot character to all the players in the room
        character_mutex.unlock();
    }
    printf("%s: Bots were added to room %d\n", get_time().c_str(), room_num);
    return true;
}

string get_time() {
    time_t curr_time = time(NULL);
    tm* local_tm = localtime(&curr_time);
    return to_string(local_tm->tm_mon + 1) + string("/") + to_string(local_tm->tm_mday) + string("/") + to_string(local_tm->tm_year + 1900)+ string(" at ") + to_string(local_tm->tm_hour) + string(":") + to_string(local_tm->tm_min) + string(":") + to_string(local_tm->tm_sec);
}

void set_socklim(int sockfd){
    socklen_t i;
    size_t len;
    size_t t1, t2;
    i = sizeof(len);
    if (getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &len, &i) < 0)
        perror(": getsockopt");
    //printf("receive buffer size = %d\n", len);

    if (getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &len, &i) < 0) 
        perror(": getsockopt");
    //printf("send buffer size = %d\n", len);
    t1 =4194300; t2 = sizeof(int);
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &t1, t2) < 0) 
        perror(": setsockopt");
    if (getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &len, &i) < 0) 
        perror(": getsockopt");
    //printf("modified receive buffer size = %d\n", len);

    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &t1, t2) < 0) 
        perror(": setsockopt");
    if (getsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &len, &i) < 0) 
        perror(": getsockopt");
    //printf("modified send buffer size = %d\n", len);
}
