/* Included inside the Agents loop in the simulator source copy. */
        {
            static char prior[240];
            char state[sizeof prior];
            int selected = gui_synclist_get_sel_pos(&list);
            char row[96];
            const char *label = selected >= 0 && selected < nrows ?
                row_name(selected, &list, row, sizeof row) : "";
            const char *const view_names[] = {
                [VIEW_SPACES] = "projects", [VIEW_AGENTS] = "threads",
                [VIEW_DETAIL] = "detail", [VIEW_QUESTION] = "question",
                [VIEW_USAGE] = "usage", [VIEW_ALERT] = "approval",
                [VIEW_INPUT] = "input", [VIEW_ACTIONS] = "actions",
                [VIEW_CONFIRM] = "confirm",
            };
            char status[48];
            const char *issue = trouble(status, sizeof status);
            snprintf(state, sizeof state, "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s",
                     global_settings.native_theme, view_names[view], selected, nrows,
                     detail_scroll, detail_scroll_max, talk_held, model.nquestions,
                     space_tab, label ? label : "", issue ? issue : "");
            if (strcmp(prior, state))
            {
                strcpy(prior, state);
                extern void t3_test_observe(const char *);
                t3_test_observe(state);
            }
        }
