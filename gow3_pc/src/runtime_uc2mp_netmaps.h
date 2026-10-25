/* The classic Uncharted 2 maps in the native net game (runtime_coop_mp.c's netinit): their levels
 * and the PS3 online spawners the Collection lost (runtime_uc2mp_netmaps.c). */
#ifndef RUNTIME_UC2MP_NETMAPS_H
#define RUNTIME_UC2MP_NETMAPS_H

/* The levels text for GOW3_COOP_MP_LEVELS' format ("<net pak>,<level>,...") of net game `game`
 * when its map is a classic map with a PS3 spawner catalogue, else NULL. Remembers the map for
 * runtime_uc2mp_netmaps_install. UC2MP_NET_GAMES_LIST=1 also prints every net game. */
const char *runtime_uc2mp_netmaps_levels(unsigned char *image, long game);
/* After the levels are applied: spawns on that map use the catalogue (1 installed, 0 not). */
int runtime_uc2mp_netmaps_install(unsigned char *image);
#endif
