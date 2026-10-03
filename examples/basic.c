#include <fbs/factions.h>
#include <stdio.h>
int main(void) {
    fbs_faction_config config = fbs_faction_config_default();
    fbs_factions *context = NULL;
    if (fbs_factions_create(&config, NULL, &context) != 0) return 1;
    printf("API version: %u\n", fbs_faction_version());
    fbs_factions_destroy(context);
    return 0;
}
