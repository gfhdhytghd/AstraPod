/* Plugin volume without persisting settings. GNU GPL version 2 or later. */

#ifdef ASTRAPOD_ROCK_COMPAT
void sound_set_volume(int value)
{
    set_volume(value, true);
}

void sound_set_transient_volume(int value)
{
    set_volume(value, false);
}
#endif
