/*
 * Copyright (c) 2005 Atheme Development Group
 * Rights to this code are documented in doc/LICENSE.
 *
 * This file contains routines to handle the HostServ HELP command.
 *
 */

#include "atheme.h"

DECLARE_MODULE_V1
(
	"hostserv/help", false, _modinit, _moddeinit,
	PACKAGE_STRING,
	"Atheme Development Group <http://www.atheme.org>"
);

list_t *hs_helptree;

static void hs_cmd_help(sourceinfo_t *si, int parc, char *parv[]);

command_t hs_help = { "HELP", N_(N_("Displays contextual help information.")), AC_NONE, 2, hs_cmd_help };

void _modinit(module_t *m)
{
	MODULE_USE_SYMBOL(hs_helptree, "hostserv/main", "hs_helptree");

	service_named_bind_command("hostserv", &hs_help);
	help_addentry(hs_helptree, "HELP", "help/help", NULL);
}

void _moddeinit()
{
	service_named_unbind_command("hostserv", &hs_help);
	help_delentry(hs_helptree, "HELP");
}

/* HELP <command> [params] */
void hs_cmd_help(sourceinfo_t *si, int parc, char *parv[])
{
	char *command = parv[0];

	if (!command)
	{
		command_success_nodata(si, _("***** \2%s Help\2 *****"), si->service->nick);
		command_success_nodata(si, _("\2%s\2 allows users to request a virtual hostname."), si->service->nick);
		command_success_nodata(si, " ");
		command_success_nodata(si, _("For more information on a command, type:"));
		command_success_nodata(si, "\2/%s%s help <command>\2", (ircd->uses_rcommand == false) ? "msg " : "", si->service->disp);
		command_success_nodata(si, " ");

		command_help(si, si->service->commands);

		command_success_nodata(si, _("***** \2End of Help\2 *****"));
		return;
	}

	/* take the command through the hash table */
	help_display(si, si->service, command, si->service->commands);
}

/* vim:cinoptions=>s,e0,n0,f0,{0,}0,^0,=s,ps,t0,c3,+s,(2s,us,)20,*30,gs,hs
 * vim:ts=8
 * vim:sw=8
 * vim:noexpandtab
 */
