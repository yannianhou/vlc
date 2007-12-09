/*****************************************************************************
 * cmdline.c: command line parsing
 *****************************************************************************
 * Copyright (C) 2001-2007 the VideoLAN team
 * $Id$
 *
 * Authors: Gildas Bazin <gbazin@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <vlc/vlc.h>
#include "../libvlc.h"
#include "vlc_keys.h"
#include "vlc_charset.h"

#include <errno.h>                                                  /* errno */

#ifdef HAVE_LIMITS_H
#   include <limits.h>
#endif

#ifdef HAVE_UNISTD_H
#    include <unistd.h>                                          /* getuid() */
#endif

#ifdef HAVE_GETOPT_LONG
#   ifdef HAVE_GETOPT_H
#       include <getopt.h>                                       /* getopt() */
#   endif
#else
#   include "../extras/getopt.h"
#endif

#if defined(HAVE_GETPWUID)
#   include <pwd.h>                                            /* getpwuid() */
#endif

#if defined( HAVE_SYS_STAT_H )
#   include <sys/stat.h>
#endif
#if defined( HAVE_SYS_TYPES_H )
#   include <sys/types.h>
#endif
#if defined( WIN32 )
#   if !defined( UNDER_CE )
#       include <direct.h>
#   endif
#include <tchar.h>
#endif

#include "config.h"
#include "modules/modules.h"

/*****************************************************************************
 * config_LoadCmdLine: parse command line
 *****************************************************************************
 * Parse command line for configuration options.
 * Now that the module_bank has been initialized, we can dynamically
 * generate the longopts structure used by getops. We have to do it this way
 * because we don't know (and don't want to know) in advance the configuration
 * options used (ie. exported) by each module.
 *****************************************************************************/
int __config_LoadCmdLine( vlc_object_t *p_this, int *pi_argc,
                          const char *ppsz_argv[],
                          vlc_bool_t b_ignore_errors )
{
    int i_cmd, i_index, i_opts, i_shortopts, flag, i_verbose = 0;
    module_t *p_parser;
    vlc_list_t *p_list;
    struct option *p_longopts;
    int i_modules_index;

    /* Short options */
    module_config_t *pp_shortopts[256];
    char *psz_shortopts;

    /* Set default configuration and copy arguments */
    p_this->p_libvlc->i_argc    = *pi_argc;
    p_this->p_libvlc->ppsz_argv = ppsz_argv;

#ifdef __APPLE__
    /* When VLC.app is run by double clicking in Mac OS X, the 2nd arg
     * is the PSN - process serial number (a unique PID-ish thingie)
     * still ok for real Darwin & when run from command line */
    if ( (*pi_argc > 1) && (strncmp( ppsz_argv[ 1 ] , "-psn" , 4 ) == 0) )
                                        /* for example -psn_0_9306113 */
    {
        /* GDMF!... I can't do this or else the MacOSX window server will
         * not pick up the PSN and not register the app and we crash...
         * hence the following kludge otherwise we'll get confused w/ argv[1]
         * being an input file name */
#if 0
        ppsz_argv[ 1 ] = NULL;
#endif
        *pi_argc = *pi_argc - 1;
        pi_argc--;
        return 0;
    }
#endif

    /* List all modules */
    p_list = vlc_list_find( p_this, VLC_OBJECT_MODULE, FIND_ANYWHERE );

    /*
     * Generate the longopts and shortopts structures used by getopt_long
     */

    i_opts = 0;
    for( i_modules_index = 0; i_modules_index < p_list->i_count;
         i_modules_index++ )
    {
        p_parser = (module_t *)p_list->p_values[i_modules_index].p_object ;

        /* count the number of exported configuration options (to allocate
         * longopts). We also need to allocate space for two options when
         * dealing with boolean to allow for --foo and --no-foo */
        i_opts += p_parser->i_config_items
                     + 2 * p_parser->i_bool_items;
    }

    p_longopts = malloc( sizeof(struct option) * (i_opts + 1) );
    if( p_longopts == NULL )
    {
        msg_Err( p_this, "out of memory" );
        vlc_list_release( p_list );
        return -1;
    }

    psz_shortopts = malloc( sizeof( char ) * (2 * i_opts + 1) );
    if( psz_shortopts == NULL )
    {
        msg_Err( p_this, "out of memory" );
        free( p_longopts );
        vlc_list_release( p_list );
        return -1;
    }

    /* If we are requested to ignore errors, then we must work on a copy
     * of the ppsz_argv array, otherwise getopt_long will reorder it for
     * us, ignoring the arity of the options */
    if( b_ignore_errors )
    {
        ppsz_argv = (const char**)malloc( *pi_argc * sizeof(char *) );
        if( ppsz_argv == NULL )
        {
            msg_Err( p_this, "out of memory" );
            free( psz_shortopts );
            free( p_longopts );
            vlc_list_release( p_list );
            return -1;
        }
        memcpy( ppsz_argv, p_this->p_libvlc->ppsz_argv,
                *pi_argc * sizeof(char *) );
    }

    i_shortopts = 0;
    for( i_index = 0; i_index < 256; i_index++ )
    {
        pp_shortopts[i_index] = NULL;
    }

    /* Fill the p_longopts and psz_shortopts structures */
    i_index = 0;
    for( i_modules_index = 0; i_modules_index < p_list->i_count;
         i_modules_index++ )
    {
        module_config_t *p_item, *p_end;
        p_parser = (module_t *)p_list->p_values[i_modules_index].p_object ;

        if( !p_parser->i_config_items )
            continue;

        for( p_item = p_parser->p_config, p_end = p_item + p_parser->confsize;
             p_item < p_end;
             p_item++ )
        {
            /* Ignore hints */
            if( p_item->i_type & CONFIG_HINT )
                continue;

            /* Add item to long options */
            p_longopts[i_index].name = strdup( p_item->psz_name );
            if( p_longopts[i_index].name == NULL ) continue;
            p_longopts[i_index].has_arg =
                (p_item->i_type == CONFIG_ITEM_BOOL)?
                                               no_argument : required_argument;
            p_longopts[i_index].flag = &flag;
            p_longopts[i_index].val = 0;
            i_index++;

            /* When dealing with bools we also need to add the --no-foo
             * option */
            if( p_item->i_type == CONFIG_ITEM_BOOL )
            {
                char *psz_name = malloc( strlen(p_item->psz_name) + 3 );
                if( psz_name == NULL ) continue;
                strcpy( psz_name, "no" );
                strcat( psz_name, p_item->psz_name );

                p_longopts[i_index].name = psz_name;
                p_longopts[i_index].has_arg = no_argument;
                p_longopts[i_index].flag = &flag;
                p_longopts[i_index].val = 1;
                i_index++;

                psz_name = malloc( strlen(p_item->psz_name) + 4 );
                if( psz_name == NULL ) continue;
                strcpy( psz_name, "no-" );
                strcat( psz_name, p_item->psz_name );

                p_longopts[i_index].name = psz_name;
                p_longopts[i_index].has_arg = no_argument;
                p_longopts[i_index].flag = &flag;
                p_longopts[i_index].val = 1;
                i_index++;
            }

            /* If item also has a short option, add it */
            if( p_item->i_short )
            {
                pp_shortopts[(int)p_item->i_short] = p_item;
                psz_shortopts[i_shortopts] = p_item->i_short;
                i_shortopts++;
                if( p_item->i_type != CONFIG_ITEM_BOOL )
                {
                    psz_shortopts[i_shortopts] = ':';
                    i_shortopts++;

                    if( p_item->i_short == 'v' )
                    {
                        psz_shortopts[i_shortopts] = ':';
                        i_shortopts++;
                    }
                }
            }
        }
    }

    /* We don't need the module list anymore */
    vlc_list_release( p_list );

    /* Close the longopts and shortopts structures */
    memset( &p_longopts[i_index], 0, sizeof(struct option) );
    psz_shortopts[i_shortopts] = '\0';

    /*
     * Parse the command line options
     */
    opterr = 0;
    optind = 0; /* set to 0 to tell GNU getopt to reinitialize */
    while( ( i_cmd = getopt_long( *pi_argc, (char **)ppsz_argv, psz_shortopts,
                                  p_longopts, &i_index ) ) != -1 )
    {
        /* A long option has been recognized */
        if( i_cmd == 0 )
        {
            module_config_t *p_conf;
            char *psz_name = (char *)p_longopts[i_index].name;

            /* Check if we deal with a --nofoo or --no-foo long option */
            if( flag ) psz_name += psz_name[2] == '-' ? 3 : 2;

            /* Store the configuration option */
            p_conf = config_FindConfig( p_this, psz_name );
            if( p_conf )
            {
                /* Check if the option is deprecated */
                if( p_conf->psz_current )
                {
                    if( p_conf->b_strict )
                    {
                        fprintf(stderr,
                                "Warning: option --%s no longer exists.\n",
                                p_conf->psz_name);
                       continue;
                    }

                    fprintf( stderr,
                             "%s: option --%s is deprecated. Use --%s instead.\n",
                             b_ignore_errors ? "Warning" : "Error",
                             p_conf->psz_name, p_conf->psz_current);
                    if( !b_ignore_errors )
                    {
                        /*free */
                        for( i_index = 0; p_longopts[i_index].name; i_index++ )
                             free( (char *)p_longopts[i_index].name );

                        free( p_longopts );
                        free( psz_shortopts );
                        return -1;
                    }

                    psz_name = (char *)p_conf->psz_current;
                    p_conf = config_FindConfig( p_this, psz_name );
                }

                switch( p_conf->i_type )
                {
                    case CONFIG_ITEM_STRING:
                    case CONFIG_ITEM_PASSWORD:
                    case CONFIG_ITEM_FILE:
                    case CONFIG_ITEM_DIRECTORY:
                    case CONFIG_ITEM_MODULE:
                    case CONFIG_ITEM_MODULE_LIST:
                    case CONFIG_ITEM_MODULE_LIST_CAT:
                    case CONFIG_ITEM_MODULE_CAT:
                        config_PutPsz( p_this, psz_name, optarg );
                        break;
                    case CONFIG_ITEM_INTEGER:
                        config_PutInt( p_this, psz_name, strtol(optarg, 0, 0));
                        break;
                    case CONFIG_ITEM_FLOAT:
                        config_PutFloat( p_this, psz_name, (float)atof(optarg) );
                        break;
                    case CONFIG_ITEM_KEY:
                        config_PutInt( p_this, psz_name, ConfigStringToKey( optarg ) );
                        break;
                    case CONFIG_ITEM_BOOL:
                        config_PutInt( p_this, psz_name, !flag );
                        break;
                }
                continue;
            }
        }

        /* A short option has been recognized */
        if( pp_shortopts[i_cmd] != NULL )
        {
            switch( pp_shortopts[i_cmd]->i_type )
            {
                case CONFIG_ITEM_STRING:
                case CONFIG_ITEM_PASSWORD:
                case CONFIG_ITEM_FILE:
                case CONFIG_ITEM_DIRECTORY:
                case CONFIG_ITEM_MODULE:
                case CONFIG_ITEM_MODULE_CAT:
                case CONFIG_ITEM_MODULE_LIST:
                case CONFIG_ITEM_MODULE_LIST_CAT:
                    config_PutPsz( p_this, pp_shortopts[i_cmd]->psz_name, optarg );
                    break;
                case CONFIG_ITEM_INTEGER:
                    if( i_cmd == 'v' )
                    {
                        if( optarg )
                        {
                            if( *optarg == 'v' ) /* eg. -vvv */
                            {
                                i_verbose++;
                                while( *optarg == 'v' )
                                {
                                    i_verbose++;
                                    optarg++;
                                }
                            }
                            else
                            {
                                i_verbose += atoi( optarg ); /* eg. -v2 */
                            }
                        }
                        else
                        {
                            i_verbose++; /* -v */
                        }
                        config_PutInt( p_this, pp_shortopts[i_cmd]->psz_name,
                                               i_verbose );
                    }
                    else
                    {
                        config_PutInt( p_this, pp_shortopts[i_cmd]->psz_name,
                                               strtol(optarg, 0, 0) );
                    }
                    break;
                case CONFIG_ITEM_BOOL:
                    config_PutInt( p_this, pp_shortopts[i_cmd]->psz_name, 1 );
                    break;
            }

            continue;
        }

        /* Internal error: unknown option */
        if( !b_ignore_errors )
        {
            fprintf( stderr, "%s: unknown option"
                     " or missing mandatory argument ",
                     p_this->p_libvlc->psz_object_name );
            if( optopt )
            {
                fprintf( stderr, "`-%c'\n", optopt );
            }
            else
            {
                fprintf( stderr, "`%s'\n", ppsz_argv[optind-1] );
            }
            fprintf( stderr, "Try `%s --help' for more information.\n",
                             p_this->p_libvlc->psz_object_name );

            for( i_index = 0; p_longopts[i_index].name; i_index++ )
                free( (char *)p_longopts[i_index].name );
            free( p_longopts );
            free( psz_shortopts );
            return -1;
        }
    }

    /* Free allocated resources */
    for( i_index = 0; p_longopts[i_index].name; i_index++ )
        free( (char *)p_longopts[i_index].name );
    free( p_longopts );
    free( psz_shortopts );
    if( b_ignore_errors ) free( ppsz_argv );

    return 0;
}

