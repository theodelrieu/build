/*
 * Copyright 1993, 1995 Christopher Seiwald.
 *
 * This file is part of Jam - see jam.c for Copyright information.
 */

# include "jam.h"
# include "lists.h"
# include "parse.h"
# include "variable.h"
# include "rules.h"
# include "newstr.h"
# include "hash.h"
# include "modules.h"

/*  This file is ALSO:
 *  (C) Copyright David Abrahams 2001. Permission to copy, use,
 *  modify, sell and distribute this software is granted provided this
 *  copyright notice appears in all copies. This software is provided
 *  "as is" without express or implied warranty, and with no claim as
 *  to its suitability for any purpose.
 */

/*
 * rules.c - access to RULEs, TARGETs, and ACTIONs
 *
 * External routines:
 *
 *    bindrule() - return pointer to RULE, creating it if necessary
 *    bindtarget() - return pointer to TARGET, creating it if necessary
 *    touchtarget() - mark a target to simulate being new
 *    targetlist() - turn list of target names into a TARGET chain
 *    targetentry() - add a TARGET to a chain of TARGETS
 *    actionlist() - append to an ACTION chain
 *    addsettings() - add a deferred "set" command to a target
 *    usesettings() - set all target specific variables
 *    pushsettings() - set all target specific variables
 *    popsettings() - reset target specific variables to their pre-push values
 *    freesettings() - delete a settings list
 *    donerules() - free RULE and TARGET tables
 *
 * 04/12/94 (seiwald) - actionlist() now just appends a single action.
 * 08/23/94 (seiwald) - Support for '+=' (append to variable)
 */

static void set_rule_actions( RULE* rule, rule_actions* actions );
static void set_rule_body( RULE* rule, argument_list* args, PARSE* procedure );
static struct hash *targethash = 0;



/*
 * enter_rule() - return pointer to RULE, creating it if necessary in
 * target_module.
 */
static RULE *
enter_rule( char *rulename, module *target_module )
{
    RULE rule, *r = &rule;

    r->name = rulename;

    if ( hashenter( target_module->rules, (HASHDATA **)&r ) )
    {
        r->name = newstr( rulename );	/* never freed */
        r->procedure = (PARSE *)0;
        r->module = 0;
        r->actions = 0;
        r->arguments = 0;
        r->exported = 0;
        r->module = target_module;
    }
    return r;
}

/*
 * define_rule() - return pointer to RULE, creating it if necessary in
 * target_module. Prepare it to accept a body or action originating in
 * src_module.
 */
static RULE *
define_rule( module *src_module, char *rulename, module *target_module )
{
    RULE *r = enter_rule( rulename, target_module );

    if ( r->module != src_module ) /* if the rule was imported from elsewhere, clear it now */
    {
        set_rule_body( r, 0, 0 ); 
        set_rule_actions( r, 0 );
        r->module = src_module; /* r will be executed in the source module */
    }

    return r;
}

/*
 * bindtarget() - return pointer to TARGET, creating it if necessary
 */

TARGET *
bindtarget( char *targetname )
{
	TARGET target, *t = &target;

	if( !targethash )
	    targethash = hashinit( sizeof( TARGET ), "targets" );

	t->name = targetname;

	if( hashenter( targethash, (HASHDATA **)&t ) )
	{
	    memset( (char *)t, '\0', sizeof( *t ) );
	    t->name = newstr( targetname );	/* never freed */
	    t->boundname = t->name;		/* default for T_FLAG_NOTFILE */
	}

	return t;
}

/*
 * touchtarget() - mark a target to simulate being new
 */

void
touchtarget( char *t )
{
	bindtarget( t )->flags |= T_FLAG_TOUCHED;
}

/*
 * targetlist() - turn list of target names into a TARGET chain
 *
 * Inputs:
 *	chain	existing TARGETS to append to
 *	targets	list of target names
 */

TARGETS *
targetlist( 
	TARGETS	*chain,
	LIST 	*targets )
{
	for( ; targets; targets = list_next( targets ) )
	    chain = targetentry( chain, bindtarget( targets->string ) );

	return chain;
}

/*
 * targetentry() - add a TARGET to a chain of TARGETS
 *
 * Inputs:
 *	chain	exisitng TARGETS to append to
 *	target	new target to append
 */

TARGETS *
targetentry( 
	TARGETS	*chain,
	TARGET	*target )
{
	TARGETS *c;

	c = (TARGETS *)malloc( sizeof( TARGETS ) );
	c->target = target;

	if( !chain ) chain = c;
	else chain->tail->next = c;
	chain->tail = c;
	c->next = 0;

	return chain;
}

/*
 * actionlist() - append to an ACTION chain
 */

ACTIONS *
actionlist(
	ACTIONS	*chain,
	ACTION	*action )
{
	ACTIONS *actions = (ACTIONS *)malloc( sizeof( ACTIONS ) );

	actions->action = action;

	if( !chain ) chain = actions;
	else chain->tail->next = actions;
	chain->tail = actions;
	actions->next = 0;

	return chain;
}

static SETTINGS* settings_freelist;

/*
 * addsettings() - add a deferred "set" command to a target
 *
 * Adds a variable setting (varname=list) onto a chain of settings
 * for a particular target.  Replaces the previous previous value,
 * if any, unless 'append' says to append the new list onto the old.
 * Returns the head of the chain of settings.
 */

SETTINGS *
addsettings(
	SETTINGS *head,
	int	append,
	char	*symbol,
	LIST	*value )
{
	SETTINGS *v;
	
	/* Look for previous setting */

	for( v = head; v; v = v->next )
	    if( !strcmp( v->symbol, symbol ) )
		break;

	/* If not previously set, alloc a new. */
	/* If appending, do so. */
	/* Else free old and set new. */

	if( !v )
	{
        v = settings_freelist;
        
        if ( v )
            settings_freelist = v->next;
        else
            v = (SETTINGS *)malloc( sizeof( *v ) );
        
	    v->symbol = newstr( symbol );
	    v->value = value;
	    v->next = head;
	    head = v;
	}
	else if( append )
	{
	    v->value = list_append( v->value, value );
	}
	else
	{
	    list_free( v->value );
	    v->value = value;
	} 

	/* Return (new) head of list. */

	return head;
}

/*
 * pushsettings() - set all target specific variables
 */

void
pushsettings( SETTINGS *v )
{
	for( ; v; v = v->next )
	    v->value = var_swap( v->symbol, v->value );
}

/*
 * popsettings() - reset target specific variables to their pre-push values
 */

void
popsettings( SETTINGS *v )
{
	pushsettings( v );	/* just swap again */
}

/*
 *    freesettings() - delete a settings list
 */

void
freesettings( SETTINGS *v )
{
	while( v )
	{
	    SETTINGS *n = v->next;

	    freestr( v->symbol );
	    list_free( v->value );
        v->next = settings_freelist;
        settings_freelist = v;

	    v = n;
	}
}

/*
 * donerules() - free TARGET tables
 */

void
donerules()
{
	hashdone( targethash );
    while ( settings_freelist )
    {
        SETTINGS* n = settings_freelist->next;
        free( settings_freelist );
        settings_freelist = n;
    }
}

/*
 * args_new() - make a new reference-counted argument list
 */
argument_list* args_new()
{
    argument_list* r = malloc( sizeof(argument_list) );
    r->reference_count = 0;
    lol_init(r->data);
    return r;
}

/*
 * args_refer() - add a new reference to the given argument list
 */
void args_refer( argument_list* a )
{
    ++a->reference_count;
}

/*
 * args_free() - release a reference to the given argument list
 */
void args_free( argument_list* a )
{
    if (--a->reference_count <= 0)
    {
        lol_free(a->data);
        free(a);
    }
}

/*
 * actions_refer() - add a new reference to the given actions
 */
void actions_refer(rule_actions* a)
{
    ++a->reference_count;
}

/*
 * actions_free() - release a reference to the given actions
 */
void actions_free(rule_actions* a)
{
    if (--a->reference_count <= 0)
    {
        freestr(a->command);
        list_free(a->bindlist);
        free(a);
    }
}

/*
 * set_rule_body() - set the argument list and procedure of the given rule
 */
void set_rule_body( RULE* rule, argument_list* args, PARSE* procedure )
{
    if ( args )
        args_refer( args );
    if ( rule->arguments )
        args_free( rule->arguments );
    rule->arguments = args;
    
    if ( procedure )
        parse_refer( procedure );
    if ( rule->procedure )
        parse_free( rule->procedure );
    rule->procedure = procedure;
}

/*
 * global_name() - given a rule, return the name for a corresponding rule in the global module
 */
static char* global_rule_name( RULE* r )
{
    if ( r->module == root_module() )
    {
        return r->name;
    }
    else
    {
        char name[4096] = "";
        strncat(name, r->module->name, sizeof(name) - 1);
        strncat(name, r->name, sizeof(name) - 1 );
        return newstr(name);
    }
}

/*
 * global_rule() - given a rule, produce the corresponding entry in the global module
 */
static RULE* global_rule( RULE* r )
{
    if ( r->module == root_module() )
    {
        return r;
    }
    else
    {
        char* name = global_rule_name( r );
        RULE* result = define_rule( r->module, name, root_module() );
        freestr(name);
        return result;
    }
}

/*
 * new_rule_body() - make a new rule named rulename in the given
 * module, with the given argument list and procedure. If exported is
 * true, the rule is exported to the global module as
 * modulename.rulename.
 */
RULE* new_rule_body( module* m, char* rulename, argument_list* args, PARSE* procedure, int exported )
{
    RULE* local = define_rule( m, rulename, m );
    local->exported = exported;
    set_rule_body( local, args, procedure );
    
    /* Mark the procedure with the global rule name, regardless of
     * whether the rule is exported. That gives us something
     * reasonably identifiable that we can use, e.g. in profiling
     * output. Only do this once, since this could be called multiple
     * times with the same procedure.
     */
    if ( procedure->rulename == 0 )
        procedure->rulename = global_rule_name( local );

    /* export the rule to the global module  if neccessary */
    if ( exported )
    {
        RULE* global = global_rule( local );
        set_rule_body( global, args, procedure );
    }

    return local;
}

static void set_rule_actions( RULE* rule, rule_actions* actions )
{
    if ( actions )
        actions_refer( actions );
    if ( rule->actions )
        actions_free( rule->actions );
    rule->actions = actions;
    
}

static rule_actions* actions_new( char* command, LIST* bindlist, int flags )
{
    rule_actions* result = malloc(sizeof(rule_actions));
    result->command = copystr( command );
    result->bindlist = bindlist;
    result->flags = flags;
    result->reference_count = 0;
    return result;
}

RULE* new_rule_actions( module* m, char* rulename, char* command, LIST* bindlist, int flags )
{
    RULE* local = define_rule( m, rulename, m );
    RULE* global = global_rule( local );
    set_rule_actions( local, actions_new( command, bindlist, flags ) );
    set_rule_actions( global, local->actions );
    return local;
}

RULE *bindrule( char *rulename, module* m )
{
    RULE rule, *r = &rule;
    r->name = rulename;
    
    if ( hashcheck( m->rules, (HASHDATA **)&r ) )
        return r;
    else
        return enter_rule( rulename, root_module() );
}

RULE* import_rule( RULE* source, module* m, char* name )
{
    RULE* dest = define_rule( source->module, name, m );
    set_rule_body( dest, source->arguments, source->procedure );
    set_rule_actions( dest, source->actions );
    return dest;
}
