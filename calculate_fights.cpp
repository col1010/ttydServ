/**
 * FIGHT CALCULATION SYSTEM:
 * When a player initiates a fight in a room, the server first checks if there are any living monsters in the current room.
 * If there are monsters, a 50/50 chance is rolled to see who gets to attack first: enemies or players
 * When players attack, a total attack value is calculated by combining the battle initiator's attack stat with every other living players' who have the auto-join battle flag set
 * When the total attack is being calculated, a +-20 offset is rolled and applied to each player's attack stat to increase variability in battles
 * For example, if a player has 300 attack, they can do anywhere from 280 to 320 damage
 * A 5% chance is then rolled for a 1.2x damage increase critical hit
 * The number of living enemies is then determined, and the total attack is divided by that number to evenly distribute the damage among the enemies
 * The damage applied to each enemy then has the enemy's defense subtracted from it
 * A player then regenerates [regen value] / 10 health (100 regen = 10 health back, 200 regen = 20 health back, etc)
 * 
 * When enemies attack, the same calculations are made
 * When enemies or players die, they no longer contribute to the damage pool and are excluded from fighting
**/

#include "lurk_structs.h"
#include<map>
#include<vector>
#include<random>
#include "send_to_clients.h"
#include <ctime>

using namespace std;

// set up random number generator
random_device rd;
mt19937 gen(rd());
uniform_int_distribution<> percent_distrib(1, 100); // create a distribution range between 1 and 100
uniform_int_distribution<> attack_distrib(-20, 20); // distribution used to vary damage, damage will be the player/monster's attack +- 20

// uses RNG to calculate chance between 1 and 99
bool percent_chance(int percent) {
    return percent_distrib(gen) <= percent; // return true if the % chance occurred, false otherwise
}

int calculate_attack(int attack) {
    attack += attack_distrib(gen); // add or subtract up to 20 attack to give a bit of variation
    if (attack < 0) // if taking off n damage put the attack below 0, bring it up to 0
        attack = 0;
    return attack;
}

void enemies_attack(character* player, map<uint16_t, vector<character*>>* room_characters_map, map<uint16_t, vector<character*>>* room_enemies_map) {
    int total_attack = 0;
    int damage_dealt;
    for (auto enemy : room_enemies_map->at(player->room_num)) {
        if (!(enemy->flags & ALIVE)) continue; // skip dead monsters
        total_attack += calculate_attack(enemy->attack); // total up all the enemies' combined attack
    }

    if (total_attack == 0) {
        send_msg_to_all_in_room(player->room_num, room_characters_map, "No Monsters available to fight! No damage dealt to players");
        return;
    }
    if (percent_chance(5)) { // 5% critical strike chance
        total_attack += total_attack * .2; // add 20% more attack
        send_msg_to_all_in_room(player->room_num, room_characters_map, (string("Critical hit! Monsters are attacking with a power of ") + to_string(total_attack)).c_str());
    } else {
        send_msg_to_all_in_room(player->room_num, room_characters_map, (string("Monsters are attacking with a power of ") + to_string(total_attack)).c_str());
    }

    int cnt = 0; // the number of battling players to split damage among
    for (auto p : room_characters_map->at(player->room_num)) { // for each player in the room
        if (!(p->flags & ALIVE)) continue; // if the player is dead, skip
        if (((p->flags & MONSTER) || !(p->flags & JOIN_BATTLE)) && p != player) continue; // if the player is a monster or does not have auto-join battles set AND they are not the one who initiated the fight 
        cnt++;
    }

    total_attack /= cnt; // spread the damage evenly among the living players
    
    for (auto p : room_characters_map->at(player->room_num)) { // for each player in the room
        if (p != player && !(p->flags & JOIN_BATTLE)) continue; // if the current player opted out of auto-join battles and they are not the one who initiated the fight, skip
        if (!(p->flags & ALIVE)) continue;
        damage_dealt = total_attack - p->defense; // damage dealt will be the attack minus a player's defense
        if (damage_dealt < 0) // if a player has more defense than incoming attack strength, return it to 0
            damage_dealt = 0;
        if (p->health - damage_dealt < -32768) // if the player's health goes below the max for an int16_t, bring it back up to avoid underflow
            p->health = -32768;
        else
            p->health -= damage_dealt; // subtract the enemies' attack from each players' health
        
        uint16_t regen = p->regen / 10;
        if (p->health < p->initial_health) { // if the player has less than INITIAL_HEALTH hp
            if (p->health + regen > p->initial_health) { // if the regen would increase the player's health above the INITIAL_HEALTH, only add enough to get to INITIAL_HEALTH
                p->health = p->initial_health;
                send_narrator_msg(p->fd, p->name, "Your regeneration healed you back to full health!");
            } else {
                p->health += regen; // add on the player's regen divided by 10
                send_narrator_msg(p->fd, p->name, (string("Your regeneration gave you ") + to_string(regen) + string(" HP!")).c_str());
            }
        }

        if (p->health <= 0) {
            send_msg_to_all_in_room(player->room_num, room_characters_map, (string(p->name) + string(" has perished!")).c_str());
            p->flags = (p->flags & (~ALIVE)); // set the player to dead
            p->health = 0;
            send_narrator_msg(p->fd, p->name, "You have fallen. But that's alright. Just send a start request and you will wake up peacefully at the inn.");
        }
    }
}

void players_attack(character* player, map<uint16_t, vector<character*>>* room_characters_map, map<uint16_t, vector<character*>>* room_enemies_map) {
    int total_attack = 0;
    int damage_dealt;

    vector<character*> tmp_vec;
    
    for (auto p : room_characters_map->at(player->room_num)) {
        if (!(p->flags & ALIVE)) continue; // if the player is dead, skip
        if (((!(p->flags & MONSTER)) || (!(p->flags & JOIN_BATTLE))) && p != player) continue; // if the character is a monster or they opted out of auto-join battles, skip unless they are the one who initiated the fight
        total_attack += calculate_attack(p->attack); // total up all the players' combined attack
        tmp_vec.push_back(p);
    }
    if (total_attack == 0) {
        send_msg_to_all_in_room(player->room_num, room_characters_map, "No players available to fight! No damage dealt to enemies");
        return;
    }
    if (percent_chance(5)) { // 5% critical strike chance
        total_attack += total_attack * .2; // add 20% more attack
        send_msg_to_all_in_room(player->room_num, room_characters_map, (string("Critical hit! Players are attacking with a power of ") + to_string(total_attack)).c_str());
    } else {
        send_msg_to_all_in_room(player->room_num, room_characters_map, (string("Players are attacking with a power of ") + to_string(total_attack)).c_str());
    }

    int cnt = 0; // the number of living enemies to split damage among
    for (auto enemy : room_enemies_map->at(player->room_num)) // for each enemy in the room
        if ((enemy->flags & ALIVE) == ALIVE) // increment the counter if the enemy is alive
            cnt++;
    
    total_attack /= cnt; // spread the damage evenly among the living enemies

    for (auto enemy : room_enemies_map->at(player->room_num)) { // for each enemy in the room
        damage_dealt = total_attack - enemy->defense; // damage dealt will be the attack minus an enemy's defense
        if (damage_dealt < 0) // if an enemy has more defense than incoming attack strength, return it to 0
            damage_dealt = 0;
        if (enemy->health - damage_dealt < -32768) // if the enemy's health goes below the max for an int16_t, bring it back up to avoid underflow
            enemy->health = -32768;
        else
            enemy->health -= damage_dealt; // subtract the enemies' attack from each players' health
        uint16_t regen = enemy->regen / 10;
        // TODO: this is wrong, INITIAL_HEALTH only applies to the players
        if (enemy->health < enemy->initial_health) { // if the enemy has less than INITIAL_HEALTH hp
            if (enemy->health + regen > enemy->initial_health) // if the regen would increase the enemy's health above its initial health, only add enough to get to initial health
                enemy->health = enemy->initial_health;
            else
                enemy->health += regen; // add on the player's regen divided by 10
        }
        if (enemy->health <= 0) {
            send_msg_to_all_in_room(enemy->room_num, room_characters_map, (string(enemy->name) + string(" has perished!")).c_str());
            enemy->flags = (enemy->flags & (~ALIVE)); // set the enemy to dead
            enemy->health = 0;
            enemy->last_active_time = time(NULL); // reset its last active timer
            uint16_t gold = enemy->gold / tmp_vec.size(); // evenly distribute the prize money
            for (auto p : tmp_vec) {
                p->gold += gold; // increase each players' gold
            }
            enemy->gold = 0; // set the enemy's gold to 0
        }
    }
}

void initiate_fight(character* player, map<uint16_t, vector<character*>>* room_characters_map, map<uint16_t, vector<character*>>* room_enemies_map) {
    
    if (!(player->flags & ALIVE)) { // if the player is dead
        send_error(player->fd, 7, "Fight cannot be initiated. You are dead.");
        return;
    }
    if (room_enemies_map->at(player->room_num).empty()) { // battle cannot be started, no enemies in the room
        send_error(player->fd, 7, "Fight cannot be initiated. No enemies in the room.");
        return;
    }
    int cnt = 0; // the number of living enemies to split damage among
    for (auto enemy : room_enemies_map->at(player->room_num)) // for each enemy in the room
        if (enemy->flags & ALIVE) // increment the counter if the enemy is alive
            cnt++;
    if (cnt == 0) {
        send_error(player->fd, 7, "Fight cannot be initiated. No living enemies.");
        return;
    }

    send_msg_to_all_in_room(player->room_num, room_characters_map, (string(player->name) + string(" initiated a fight!")).c_str());
    if (percent_chance(50)) { // 50/50 chance on who attacks first, enemies or players
        // enemies attack first
        send_msg_to_all_in_room(player->room_num, room_characters_map, "Enemies are attacking first!");
        enemies_attack(player, room_characters_map, room_enemies_map);
        players_attack(player, room_characters_map, room_enemies_map);
    } else {
        // players attack first
        send_msg_to_all_in_room(player->room_num, room_characters_map, "Players are attacking first!");
        players_attack(player, room_characters_map, room_enemies_map);
        enemies_attack(player, room_characters_map, room_enemies_map);
    }
    for (auto c : room_characters_map->at(player->room_num)) { // for every character in the room
        if (c->fd == -1) continue; // skip disconnected players and NPCs
        send_characters_in_room(c->fd, room_characters_map, player->room_num); // send all the updated characters
    }
    return;

}
