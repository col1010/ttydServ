#include<map>
#include<vector>
#include<string>

using namespace std;

bool send_message(int fd, const char* sender, const char* recipient, const char* message);
bool send_narrator_msg(int fd, const char* recipient, const char* message);
bool send_game(int fd);
bool send_version(int fd);
bool send_error(int fd, uint8_t err_code, const char* err_msg);
bool send_room(int fd, room* r);
bool send_connections(int fd, std::map<uint16_t, room>* room_map, std::vector<uint16_t>* connections);
bool send_msg_to_all(map<string, character*>* character_map, const char* msg);
bool send_msg_to_all_in_room(uint16_t room_num, map<uint16_t, vector<character*>>* room_characters_map, const char * msg);
bool send_accept(int fd, uint8_t acc_type);
bool send_characters_in_room(int fd, map<uint16_t, vector<character*>>* room_characters_map, uint16_t room_num);
bool send_character(int fd, character* ch);
bool send_ch_to_all_in_room(character* character_to_send, map<uint16_t, vector<character*>>* room_characters_map, uint16_t room_num);
