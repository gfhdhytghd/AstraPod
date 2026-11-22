
enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    MENUITEM_STRINGLIST(start, "Battery Benchmark", NULL,
                        "Start logging", "Quit");
    MENUITEM_STRINGLIST(stop, "Battery Benchmark", NULL,
                        "Stop logging", "Quit");

    bool running = rb->rock_battery_bench_running();
    int selection = 0;
    int item = rb->do_menu(running ? &stop : &start, &selection, NULL, false);
    if (item == MENU_ATTACHED_USB)
        return PLUGIN_USB_CONNECTED;
    if (item != 0)
        return PLUGIN_OK;

    bool ok = running ? rb->rock_battery_bench_stop() :
                        rb->rock_battery_bench_start();
    rb->splash(HZ, ok ? "Battery log: /battery_bench.txt" :
                        "Could not write battery log");
    return ok ? PLUGIN_OK : PLUGIN_ERROR;
}
