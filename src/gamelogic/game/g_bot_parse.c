/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Daemon.

Daemon is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Daemon is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "g_bot_parse.h"
#include "g_bot_util.h"

static void CheckToken( const char *tokenValueName, const char *nodename, const pc_token_t *token, int requiredType )
{
	if ( token->type != requiredType )
	{
		BotDPrintf( S_COLOR_RED "ERROR: Invalid %s %s after %s on line %d\n", tokenValueName, token->string, nodename, token->line );
	}
}

static qboolean expectToken( const char *s, pc_token_list **list, qboolean next )
{
	const pc_token_list *current = *list;

	if ( !current )
	{
		BotError( "Expected token %s but found end of file\n", s );
		return qfalse;
	}
	
	if ( Q_stricmp( current->token.string, s ) != 0 )
	{
		BotError( "Expected token %s but found %s on line %d\n", s, current->token.string, current->token.line );
		return qfalse;
	}
	
	if ( next )
	{
		*list = current->next;
	}
	return qtrue;
}

// functions that are used to provide values to the behavior tree in condition nodes
static AIValue_t buildingIsDamaged( gentity_t *self, const AIValue_t *params )
{
	return AIBoxInt( self->botMind->closestDamagedBuilding.ent != NULL );
}

static AIValue_t haveWeapon( gentity_t *self, const AIValue_t *params )
{
	return AIBoxInt( BG_InventoryContainsWeapon( AIUnBoxInt( params[ 0 ] ), self->client->ps.stats ) );
}

static AIValue_t alertedToEnemy( gentity_t *self, const AIValue_t *params )
{
	qboolean success;
	if ( level.time - self->botMind->timeFoundEnemy < g_bot_reactiontime.integer )
	{
		success = qfalse;
	}
	else if ( !self->botMind->bestEnemy.ent )
	{
		success = qfalse;
	}
	else
	{
		success = qtrue;
	}

	return AIBoxInt( success );
}

static AIValue_t botTeam( gentity_t *self, const AIValue_t *params )
{
	return AIBoxInt( self->client->ps.stats[ STAT_TEAM ] );
}

static AIValue_t currentWeapon( gentity_t *self, const AIValue_t *params )
{
	return AIBoxInt( self->client->ps.weapon == AIUnBoxInt( params[ 0 ] ) );
}

static AIValue_t haveUpgrade( gentity_t *self, const AIValue_t *params )
{
	int upgrade = AIUnBoxInt( params[ 0 ] );
	return AIBoxInt( !BG_UpgradeIsActive( upgrade, self->client->ps.stats ) && BG_InventoryContainsUpgrade( upgrade, self->client->ps.stats ) );
}
static AIValue_t botHealth( gentity_t *self, const AIValue_t *params )
{
	float health = self->health;
	float maxHealth = BG_Class( self->client->ps.stats[ STAT_CLASS ] )->health;
	return AIBoxFloat( health / maxHealth );
}

static AIValue_t botAmmo( gentity_t *self, const AIValue_t *params )
{
	return AIBoxFloat( PercentAmmoRemaining( BG_PrimaryWeapon( self->client->ps.stats ), &self->client->ps ) );
}

static AIValue_t teamateHasWeapon( gentity_t *self, const AIValue_t *params )
{
	return AIBoxInt( BotTeamateHasWeapon( self, AIUnBoxInt( params[ 0 ] ) ) );
}

static AIValue_t distanceTo( gentity_t *self, const AIValue_t *params )
{
	AIEntity_t e = ( AIEntity_t ) AIUnBoxInt( params[ 0 ] );
	float distance = 0;
	botEntityAndDistance_t *ent = AIEntityToGentity( self, e );

	if ( ent )
	{
		distance = ent->distance;
	}
	else if ( e == E_GOAL )
	{
		distance = DistanceToGoal( self );
	}
	
	return AIBoxFloat( distance );
}

static AIValue_t baseRushScore( gentity_t *self, const AIValue_t *params )
{
	return AIBoxFloat( BotGetBaseRushScore( self ) );
}

static AIValue_t healScore( gentity_t *self, const AIValue_t *params )
{
	return AIBoxFloat( BotGetHealScore( self ) );
}

static AIValue_t botClass( gentity_t *self, const AIValue_t *params )
{
	return AIBoxInt( self->client->ps.stats[ STAT_CLASS ] );
}

// functions accessible to the behavior tree for use in condition nodes
static const struct AIConditionMap_s
{
	const char    *name;
	AIValueType_t retType;
	AIFunc        func;
	int           nparams;
} conditionFuncs[] =
{
	{ "alertedToEnemy",    VALUE_INT,   alertedToEnemy,    0 },
	{ "baseRushScore",     VALUE_FLOAT, baseRushScore,     0 },
	{ "buildingIsDamaged", VALUE_INT,   buildingIsDamaged, 0 },
	{ "class",             VALUE_INT,   botClass,          0 },
	{ "distanceTo",        VALUE_FLOAT, distanceTo,        1 },
	{ "haveUpgrade",       VALUE_INT,   haveUpgrade,       1 },
	{ "haveWeapon",        VALUE_INT,   haveWeapon,        1 },
	{ "healScore",         VALUE_FLOAT, healScore,         0 },
	{ "percentHealth",     VALUE_FLOAT, botHealth,         0 },
	{ "team",              VALUE_INT,   botTeam,           0 },
	{ "teamateHasWeapon",  VALUE_INT,   teamateHasWeapon,  1 },
	{ "weapon",            VALUE_INT,   currentWeapon,     0 }
};

static const struct AIOpMap_s
{
	const char            *str;
	int                   tokenSubtype;
	AIOpType_t            opType;
} conditionOps[] =
{
	{ ">=", P_LOGIC_GEQ,     OP_GREATERTHANEQUAL },
	{ ">",  P_LOGIC_GREATER, OP_GREATERTHAN      },
	{ "<=", P_LOGIC_LEQ,     OP_LESSTHANEQUAL    },
	{ "<",  P_LOGIC_LESS,    OP_LESSTHAN         },
	{ "==", P_LOGIC_EQ,      OP_EQUAL            },
	{ "!=", P_LOGIC_UNEQ,    OP_NEQUAL           },
	{ "!",  P_LOGIC_NOT,     OP_NOT              },
	{ "&&", P_LOGIC_AND,     OP_AND              },
	{ "||", P_LOGIC_OR,      OP_OR               }
};

static AIOpType_t opTypeFromToken( pc_token_t *token )
{
	int i;
	if ( token->type != TT_PUNCTUATION )
	{
		return OP_NONE;
	}

	for ( i = 0; i < ARRAY_LEN( conditionOps ); i++ )
	{
		if ( token->subtype == conditionOps[ i ].tokenSubtype )
		{
			return conditionOps[ i ].opType;
		}
	}
	return OP_NONE;
}

static const char *opTypeToString( AIOpType_t op )
{
	int i;
	for ( i = 0; i < ARRAY_LEN( conditionOps ); i++ )
	{
		if ( conditionOps[ i ].opType == op )
		{
			return conditionOps[ i ].str;
		}
	}
	return NULL;
}

// compare operator precedence
static int opCompare( AIOpType_t op1, AIOpType_t op2 )
{
	if ( op1 < op2 )
	{
		return 1;
	}
	else if ( op1 < op2 )
	{
		return -1;
	}
	return 0;
}

static pc_token_list *findCloseParen( pc_token_list *start, pc_token_list *end )
{
	pc_token_list *list = start;
	int depth = 0;

	while ( list != end )
	{
		if ( list->token.string[ 0 ] == '(' )
		{
			depth++;
		}

		if ( list->token.string[ 0 ] == ')' )
		{
			depth--;
		}

		if ( depth == 0 )
		{
			return list;
		}

		list = list->next;
	}

	return NULL;
}

static AIOp_t *newOp( pc_token_list *list )
{
	pc_token_list *current = list;
	AIOp_t *ret = NULL;

	AIOpType_t op = opTypeFromToken( &current->token );

	if ( isBinaryOp( op ) )
	{
		AIBinaryOp_t *b = ( AIBinaryOp_t * ) BG_Alloc( sizeof( *b ) );
		b->opType = op;
		ret = ( AIOp_t * ) b;
	}
	else if ( isUnaryOp( op ) )
	{
		AIUnaryOp_t *u = ( AIUnaryOp_t * ) BG_Alloc( sizeof( *u ) );
		u->opType = op;
		ret = ( AIOp_t * ) u;
	}

	return ret;
}

static AIValue_t *newValueLiteral( pc_token_list **list )
{
	AIValue_t *ret;
	pc_token_list *current = *list;
	pc_token_t *token = &current->token;

	ret = ( AIValue_t * ) BG_Alloc( sizeof( *ret ) );

	*ret = AIBoxToken( token );

	*list = current->next;
	return ret;
}

static AIValue_t *parseFunctionParameters( pc_token_list **list, int *nparams, int minparams, int maxparams )
{
	pc_token_list *current = *list;
	pc_token_list *parenBegin = current->next;
	pc_token_list *parenEnd;
	pc_token_list *parse;
	AIValue_t     *params;
	int           numParams = 0;

	// functions should always be proceeded by a '(' if they have parameters
	if ( !expectToken( "(", &parenBegin, qfalse ) )
	{
		*list = current;
		return NULL;
	}

	// find the end parenthesis around the function's args
	parenEnd = findCloseParen( parenBegin, NULL );

	if ( !parenEnd )
	{
		BotError( "could not find matching ')' for '(' on line %d", parenBegin->token.line );
		*list = parenBegin->next;
		return NULL;
	}

	// count the number of parameters
	parse = parenBegin->next;

	while ( parse != parenEnd )
	{
		if ( parse->token.type == TT_NUMBER )
		{
			numParams++;
		}
		else if ( parse->token.string[ 0 ] != ',' )
		{
			BotError( "Invalid token %s in parameter list on line %d\n", parse->token.string, parse->token.line );
			*list = parenEnd->next; // skip invalid function expression
			return NULL;
		}
		parse = parse->next;
	}

	// warn if too many or too few parameters
	if ( numParams < minparams )
	{
		BotError( "too few parameters for %s on line %d\n", current->token.string, current->token.line );
		*list = parenEnd->next;
		return NULL;
	}

	if ( numParams > maxparams )
	{
		BotError( "too many parameters for %s on line %d\n", current->token.string, current->token.line );
		*list = parenEnd->next;
		return NULL;
	}

	*nparams = numParams;

	if ( numParams )
	{
		// add the parameters
		params = ( AIValue_t * ) BG_Alloc( sizeof( *params ) * numParams );

		numParams = 0;
		parse = parenBegin->next;
		while ( parse != parenEnd )
		{
			if ( parse->token.type == TT_NUMBER )
			{
				params[ numParams ] = AIBoxToken( &parse->token );
				numParams++;
			}
			parse = parse->next;
		}
	}
	*list = parenEnd->next;
	return params;
}

static AIValueFunc_t *newValueFunc( pc_token_list **list )
{
	AIValueFunc_t *ret = NULL;
	AIValueFunc_t v;
	pc_token_list *current = *list;
	pc_token_list *parenBegin = NULL;
	struct AIConditionMap_s *f;

	memset( &v, 0, sizeof( v ) );

	f = bsearch( current->token.string, conditionFuncs, ARRAY_LEN( conditionFuncs ), sizeof( *conditionFuncs ), cmdcmp );

	if ( !f )
	{
		BotError( "Unknown function: %s on line %d\n", current->token.string, current->token.line );
		*list = current->next;
		return NULL;
	}

	v.expType = EX_FUNC;
	v.retType = f->retType;
	v.func =    f->func;
	v.nparams = f->nparams;

	parenBegin = current->next;

	// if the function has no parameters, allow it to be used without parenthesis
	if ( v.nparams == 0 && parenBegin->token.string[ 0 ] != '(' )
	{
		ret = ( AIValueFunc_t * ) BG_Alloc( sizeof( *ret ) );
		memcpy( ret, &v, sizeof( *ret ) );

		*list = current->next;
		return ret;
	}

	v.params = parseFunctionParameters( list, &v.nparams, f->nparams, f->nparams );

	if ( !v.params && f->nparams > 0 )
	{
		return NULL;
	}

	// create the value op
	ret = ( AIValueFunc_t * ) BG_Alloc( sizeof( *ret ) );

	// copy the members
	memcpy( ret, &v, sizeof( *ret ) );

	return ret;
}

static AIExpType_t *makeExpression( AIOp_t *op, AIExpType_t *exp1, AIExpType_t *exp2 )
{
	if ( isUnaryOp( op->opType ) )
	{
		AIUnaryOp_t *u = ( AIUnaryOp_t * ) op;
		u->exp = exp1;
	}
	else if ( isBinaryOp( op->opType ) )
	{
		AIBinaryOp_t *b = ( AIBinaryOp_t * ) op;
		b->exp1 = exp1;
		b->exp2 = exp2;
	}

	return ( AIExpType_t * ) op;
}

static AIExpType_t *Primary( pc_token_list **list );
static AIExpType_t *ReadConditionExpression( pc_token_list **list, AIOpType_t op2 )
{
	AIExpType_t *t;
	AIOpType_t  op;

	if ( !*list )
	{
		BotError( "Unexpected end of file\n" );
		return NULL;
	}

	t = Primary( list );

	if ( !t )
	{
		return NULL;
	}

	op = opTypeFromToken( &(*list)->token );

	while ( isBinaryOp( op ) && opCompare( op, op2 ) >= 0 )
	{
		AIExpType_t *t1;
		pc_token_list *prev = *list;
		AIOp_t *exp = newOp( *list );
		*list = (*list)->next;
		t1 = ReadConditionExpression( list, op );

		if ( !t1 )
		{
			BotError( "Missing right operand for %s on line %d\n", opTypeToString( op ), prev->token.line );
			FreeExpression( t );
			FreeOp( exp );
			return NULL;
		}

		t = makeExpression( exp, t, t1 );

		op = opTypeFromToken( &(*list)->token );
	}

	return t;
}

static AIExpType_t *Primary( pc_token_list **list )
{
	pc_token_list *current = *list;
	AIExpType_t *tree = NULL;

	if ( isUnaryOp( opTypeFromToken( &current->token ) ) )
	{
		AIExpType_t *t;
		AIOp_t *op = newOp( current );
		*list = current->next;
		t = ReadConditionExpression( list, op->opType );

		if ( !t )
		{
			BotError( "Missing right operand for %s on line %d\n", opTypeToString( op->opType ), current->token.line );
			FreeOp( op );
			return NULL;
		}

		tree = makeExpression( op, t, NULL );
	}
	else if ( current->token.string[0] == '(' )
	{
		*list = current->next;
		tree = ReadConditionExpression( list, OP_NONE );
		if ( !expectToken( ")", list, qtrue ) )
		{
			return NULL;
		}
	}
	else if ( current->token.type == TT_NUMBER )
	{
		tree = ( AIExpType_t * ) newValueLiteral( list );
	}
	else if ( current->token.type == TT_NAME )
	{
		tree = ( AIExpType_t * ) newValueFunc( list );
	}
	else
	{
		BotError( "token %s on line %d is not valid\n", current->token.string, current->token.line );
	}
	return tree;
}

static void BotInitNode( AINode_t type, AINodeRunner func, void *node )
{
	AIGenericNode_t *n = ( AIGenericNode_t * ) node;
	n->type = type;
	n->run = func;
}

/*
======================
ReadConditionNode

Parses and creates an AIConditionNode_t from a token list
The token list pointer is modified to point to the beginning of the next node text block

A condition node has the form:
condition [expression] {
	child node
}

or the form:
condition [expression]

[expression] can be any valid set of boolean operations and values
======================
*/
AIGenericNode_t *ReadConditionNode( pc_token_list **tokenlist )
{
	pc_token_list *current = *tokenlist;

	AIConditionNode_t *condition;

	if ( !expectToken( "condition", &current, qtrue ) )
	{
		return NULL;
	}

	condition = allocNode( AIConditionNode_t );
	BotInitNode( CONDITION_NODE, BotConditionNode, condition );

	condition->exp = ReadConditionExpression( &current, OP_NONE );

	if ( !current )
	{
		*tokenlist = current;
		BotError( "Unexpected end of file\n" );
		FreeConditionNode( condition );
		return NULL;
	}

	if ( !condition->exp )
	{
		*tokenlist = current;
		FreeConditionNode( condition );
		return NULL;
	}

	if ( Q_stricmp( current->token.string, "{" ) )
	{
		// this condition node has no child nodes
		*tokenlist = current;
		return ( AIGenericNode_t * ) condition;
	}

	current = current->next;

	condition->child = ReadNode( &current );

	if ( !condition->child )
	{
		BotError( "Failed to parse child node of condition on line %d\n", (*tokenlist)->token.line );
		*tokenlist = current;
		FreeConditionNode( condition );
		return NULL;
	}

	if ( !expectToken( "}", &current, qtrue ) )
	{
		*tokenlist = current;
		FreeConditionNode( condition );
		return NULL;
	}

	*tokenlist = current;

	return ( AIGenericNode_t * ) condition;
}

static const struct AIActionMap_s
{
	const char   *name;
	AINodeRunner run;
	int          minparams;
	int          maxparams;
} AIActions[] =
{
	{ "buy",          BotActionBuy,          1, 4 },
	{ "equip",        BotActionBuy,          0, 0 },
	{ "evolve",       BotActionEvolve,       0, 0 },
	{ "fight",        BotActionFight,        0, 0 },
	{ "flee",         BotActionFlee,         0, 0 },
	{ "heal",         BotActionHeal,         0, 0 },
	{ "moveTo",       BotActionMoveTo,       1, 2 },
	{ "repair",       BotActionRepair,       0, 0 },
	{ "roam",         BotActionRoam,         0, 0 },
	{ "roamInRadius", BotActionRoamInRadius, 2, 2 },
	{ "rush",         BotActionRush,         0, 0 }
};

/*
======================
ReadActionNode

Parses and creates an AIGenericNode_t with the type ACTION_NODE from a token list
The token list pointer is modified to point to the beginning of the next node text block after reading

An action node has the form:
action name( p1, p2, ... )

Where name defines the action to execute, and the parameters are surrounded by parenthesis
======================
*/
AIGenericNode_t *ReadActionNode( pc_token_list **tokenlist )
{
	pc_token_list *current = *tokenlist;
	pc_token_list *parenBegin;
	AIActionNode_t        *ret = NULL;
	AIActionNode_t        node;
	struct AIActionMap_s  *action = NULL;

	if ( !expectToken( "action", &current, qtrue ) )
	{
		return NULL;
	}

	if ( !current )
	{
		BotError( "Unexpected end of file after line %d\n", (*tokenlist)->token.line );
		return NULL;
	}

	action = bsearch( current->token.string, AIActions, ARRAY_LEN( AIActions ), sizeof( *AIActions ), cmdcmp );

	if ( !action )
	{
		BotError( "%s on line %d is not a valid action\n", current->token.string, current->token.line );
		*tokenlist = current;
		return NULL;
	}

	// if this action doesn't have any parameters, save memory by allocating a small AIGenericNode_t
	if ( action->minparams == 0 && action->maxparams == 0 )
	{
		ret = ( AIActionNode_t * ) allocNode(  AIGenericNode_t );
		BotInitNode( ACTION_NODE, action->run, ret );
		*tokenlist = current->next;
		return ( AIGenericNode_t * ) ret;
	}

	parenBegin = current->next;

	memset( &node, 0, sizeof( node ) );

	BotInitNode( ACTION_NODE, action->run, &node );

	// allow dropping of parenthesis if we don't require any parameters
	if ( action->minparams == 0 && parenBegin->token.string[0] != '(' )
	{
		ret = allocNode( AIActionNode_t );
		memcpy( ret, &node, sizeof( node ) );
		*tokenlist = parenBegin;
		return ( AIGenericNode_t * ) ret;
	}

	node.params = parseFunctionParameters( &current, &node.nparams, action->minparams, action->maxparams );

	if ( !node.params && action->minparams > 0 )
	{
		return NULL;
	}

	// create the action node
	ret = allocNode( AIActionNode_t );
	memcpy( ret, &node, sizeof( *ret ) );

	*tokenlist = current;
	return ( AIGenericNode_t * ) ret;
}

/*
======================
ReadNodeList

Parses and creates an AINodeList_t from a token list
The token list pointer is modified to point to the beginning of the next node text block after reading

A node list has one of these forms:
selector sequence {
[ one or more child nodes ]
}

selector priority {
[ one or more child nodes ]
}

selector {
[ one or more child nodes ]
}
======================
*/
AIGenericNode_t *ReadNodeList( pc_token_list **tokenlist )
{
	AINodeList_t *list;
	pc_token_list *current = *tokenlist;

	if ( !expectToken( "selector", &current, qtrue ) )
	{
		return NULL;
	}

	list = allocNode( AINodeList_t );

	if ( !Q_stricmp( current->token.string, "sequence" ) )
	{
		BotInitNode( SELECTOR_NODE, BotSequenceNode, list );
		current = current->next;
	}
	else if ( !Q_stricmp( current->token.string, "priority" ) )
	{
		BotInitNode( SELECTOR_NODE, BotPriorityNode, list );
		current = current->next;
	}
	else if ( !Q_stricmp( current->token.string, "{" ) )
	{
		BotInitNode( SELECTOR_NODE, BotSelectorNode, list );
	}
	else
	{
		BotError( "Invalid token %s on line %d\n", current->token.string, current->token.line );
		FreeNodeList( list );
		*tokenlist = current;
		return NULL;
	}

	if ( !expectToken( "{", &current, qtrue ) )
	{
		FreeNodeList( list );
		return NULL;
	}

	while ( Q_stricmp( current->token.string, "}" ) )
	{
		AIGenericNode_t *node = ReadNode( &current );

		if ( node && list->numNodes >= MAX_NODE_LIST )
		{
			BotError( "Max selector children limit exceeded at line %d\n", (*tokenlist)->token.line );
			FreeNode( node );
			FreeNodeList( list );
			*tokenlist = current;
			return NULL;
		}
		else if ( node )
		{
			list->list[ list->numNodes ] = node;
			list->numNodes++;
		}

		if ( !node )
		{
			*tokenlist = current;
			FreeNodeList( list );
			return NULL;
		}

		if ( !current )
		{
			*tokenlist = current;
			return ( AIGenericNode_t * ) list;
		}
	}

	*tokenlist = current->next;
	return ( AIGenericNode_t * ) list;
}

/*
======================
ReadNode

Parses and creates an AIGenericNode_t from a token list
The token list pointer is modified to point to the next node text block after reading

This function delegates the reading to the sub functions
ReadNodeList, ReadActionNode, and ReadConditionNode depending on the first token in the list
======================
*/

AIGenericNode_t *ReadNode( pc_token_list **tokenlist )
{
	pc_token_list *current = *tokenlist;
	AIGenericNode_t *node;

	if ( !Q_stricmp( current->token.string, "selector" ) )
	{
		node = ReadNodeList( &current );
	}
	else if ( !Q_stricmp( current->token.string, "action" ) )
	{
		node = ReadActionNode( &current );
	}
	else if ( !Q_stricmp( current->token.string, "condition" ) )
	{
		node = ReadConditionNode( &current );
	}
	else
	{
		BotError( "Invalid token on line %d found: %s\n", current->token.line, current->token.string );
		node = NULL;
	}

	*tokenlist = current;
	return node;
}

/*
======================
ReadBehaviorTree

Load a behavior tree of the given name from a file
======================
*/
AIBehaviorTree_t *ReadBehaviorTree( const char *name, AITreeList_t *list )
{
	int i;
	char treefilename[ MAX_QPATH ];
	int handle;
	pc_token_list *tokenlist;
	AIBehaviorTree_t *tree;
	pc_token_list *current;
	AIGenericNode_t *node;

	// check if this behavior tree has already been loaded
	for ( i = 0; i < list->numTrees; i++ )
	{
		AIBehaviorTree_t *tree = list->trees[ i ];
		if ( !Q_stricmp( tree->name, name ) )
		{
			return tree;
		}
	}

	// add preprocessor defines for use in the behavior tree
	// add upgrades
	D( UP_LIGHTARMOUR );
	D( UP_HELMET );
	D( UP_MEDKIT );
	D( UP_BATTPACK );
	D( UP_JETPACK );
	D( UP_BATTLESUIT );
	D( UP_GRENADE );

	// add weapons
	D( WP_MACHINEGUN );
	D( WP_PAIN_SAW );
	D( WP_SHOTGUN );
	D( WP_LAS_GUN );
	D( WP_MASS_DRIVER );
	D( WP_CHAINGUN );
	D( WP_FLAMER );
	D( WP_PULSE_RIFLE );
	D( WP_LUCIFER_CANNON );
	D( WP_GRENADE );
	D( WP_HBUILD );

	// add teams
	D( TEAM_ALIENS );
	D( TEAM_HUMANS );

	// add AIEntitys
	D( E_NONE );
	D( E_A_SPAWN );
	D( E_A_OVERMIND );
	D( E_A_BARRICADE );
	D( E_A_ACIDTUBE );
	D( E_A_TRAPPER );
	D( E_A_BOOSTER );
	D( E_A_HIVE );
	D( E_H_SPAWN );
	D( E_H_MGTURRET );
	D( E_H_TESLAGEN );
	D( E_H_ARMOURY );
	D( E_H_DCC );
	D( E_H_MEDISTAT );
	D( E_H_REACTOR );
	D( E_H_REPEATER );
	D( E_GOAL );
	D( E_ENEMY );
	D( E_DAMAGEDBUILDING );

	// add player classes
	D( PCL_NONE );
	D( PCL_ALIEN_BUILDER0 );
	D( PCL_ALIEN_BUILDER0_UPG );
	D( PCL_ALIEN_LEVEL0 );
	D( PCL_ALIEN_LEVEL1 );
	D( PCL_ALIEN_LEVEL1_UPG );
	D( PCL_ALIEN_LEVEL2 );
	D( PCL_ALIEN_LEVEL2_UPG );
	D( PCL_ALIEN_LEVEL3 );
	D( PCL_ALIEN_LEVEL3_UPG );
	D( PCL_ALIEN_LEVEL4 );
	D( PCL_HUMAN );
	D( PCL_HUMAN_BSUIT );
	
	Q_strncpyz( treefilename, va( "bots/%s.bt", name ), sizeof( treefilename ) );

	handle = trap_Parse_LoadSource( treefilename );
	if ( !handle )
	{
		G_Printf( S_COLOR_RED "ERROR: Cannot load behavior tree %s: File not found\n", treefilename );
		return NULL;
	}

	tokenlist = CreateTokenList( handle );
	
	tree = ( AIBehaviorTree_t * ) BG_Alloc( sizeof( AIBehaviorTree_t ) );

	Q_strncpyz( tree->name, name, sizeof( tree->name ) );

	current = tokenlist;

	node = ReadNode( &current );
	if ( node )
	{
		tree->root = ( AINode_t * ) node;
	}
	else
	{
		BG_Free( tree );
		tree = NULL;
	}

	if ( tree )
	{
		AddTreeToList( list, tree );
	}

	FreeTokenList( tokenlist );
	trap_Parse_FreeSource( handle );
	return tree;
}

pc_token_list *CreateTokenList( int handle )
{
	pc_token_t token;
	char filename[ MAX_QPATH ];
	pc_token_list *current = NULL;
	pc_token_list *root = NULL;

	while ( trap_Parse_ReadToken( handle, &token ) )
	{
		pc_token_list *list = ( pc_token_list * ) BG_Alloc( sizeof( pc_token_list ) );
		
		if ( current )
		{
			list->prev = current;
			current->next = list;
		}
		else
		{
			list->prev = list;
			root = list;
		}
		
		current = list;
		current->next = NULL;

		current->token = token;
		trap_Parse_SourceFileAndLine( handle, filename, &current->token.line );
	}

	return root;
}

void FreeTokenList( pc_token_list *list )
{
	pc_token_list *current = list;
	while( current )
	{
		pc_token_list *node = current;
		current = current->next;

		BG_Free( node );
	}
}

// functions for keeping a list of behavior trees loaded
void InitTreeList( AITreeList_t *list )
{
	list->trees = ( AIBehaviorTree_t ** ) BG_Alloc( sizeof( AIBehaviorTree_t * ) * 10 );
	list->maxTrees = 10;
	list->numTrees = 0;
}

void AddTreeToList( AITreeList_t *list, AIBehaviorTree_t *tree )
{
	if ( list->maxTrees == list->numTrees )
	{
		AIBehaviorTree_t **trees = ( AIBehaviorTree_t ** ) BG_Alloc( sizeof( AIBehaviorTree_t * ) * list->maxTrees );
		list->maxTrees *= 2;
		memcpy( trees, list->trees, sizeof( AIBehaviorTree_t * ) * list->numTrees );
		BG_Free( list->trees );
		list->trees = trees;
	}

	list->trees[ list->numTrees ] = tree;
	list->numTrees++;
}

void RemoveTreeFromList( AITreeList_t *list, AIBehaviorTree_t *tree )
{
	int i;

	for ( i = 0; i < list->numTrees; i++ )
	{
		AIBehaviorTree_t *testTree = list->trees[ i ];
		if ( !Q_stricmp( testTree->name, tree->name ) )
		{
			FreeBehaviorTree( testTree );
			memmove( &list->trees[ i ], &list->trees[ i + 1 ], sizeof( AIBehaviorTree_t * ) * ( list->numTrees - i - 1 ) );
			list->numTrees--;
		}
	}
}

void FreeTreeList( AITreeList_t *list )
{
	int i;
	for ( i = 0; i < list->numTrees; i++ )
	{
		AIBehaviorTree_t *tree = list->trees[ i ];
		FreeBehaviorTree( tree );
	}

	BG_Free( list->trees );
	list->trees = NULL;
	list->maxTrees = 0;
	list->numTrees = 0;
}

// functions for freeing the memory of condition expressions
void FreeValue( AIValue_t *v )
{
	if ( !v )
	{
		return;
	}

	BG_Free( v );
}

void FreeValueFunc( AIValueFunc_t *v )
{
	if ( !v )
	{
		return;
	}

	BG_Free( v->params );
	BG_Free( v );
}

void FreeExpression( AIExpType_t *exp )
{
	if ( !exp )
	{
		return;
	}

	if ( *exp == EX_FUNC )
	{
		AIValueFunc_t *v = ( AIValueFunc_t * ) exp;
		FreeValueFunc( v );
	}
	else if ( *exp == EX_VALUE )
	{
		AIValue_t *v = ( AIValue_t * ) exp;
		
		FreeValue( v );
	}
	else if ( *exp == EX_OP )
	{
		AIOp_t *op = ( AIOp_t * ) exp;

		FreeOp( op );
	}
}
void FreeOp( AIOp_t *op )
{
	if ( !op )
	{
		return;
	}

	if ( isBinaryOp( op->opType ) )
	{
		AIBinaryOp_t *b = ( AIBinaryOp_t * ) op;
		FreeExpression( b->exp1 );
		FreeExpression( b->exp2 );
	}
	else if ( isUnaryOp( op->opType ) )
	{
		AIUnaryOp_t *u = ( AIUnaryOp_t * ) op;
		FreeExpression( u->exp );
	}

	BG_Free( op );
}

// freeing behavior tree nodes
void FreeConditionNode( AIConditionNode_t *node )
{
	FreeNode( node->child );
	FreeExpression( node->exp );
	BG_Free( node );
}

void FreeNodeList( AINodeList_t *node )
{
	int i;
	for ( i = 0; i < node->numNodes; i++ )
	{
		FreeNode( node->list[ i ] );
	}
	BG_Free( node );
}

void FreeNode( AIGenericNode_t *node )
{
	if ( !node )
	{
		return;
	}

	if ( node->type == SELECTOR_NODE )
	{
		FreeNodeList( ( AINodeList_t * ) node );
	}
	else if ( node->type == CONDITION_NODE )
	{
		FreeConditionNode( ( AIConditionNode_t * ) node );
	}
	else if ( node->type == ACTION_NODE )
	{
		BG_Free( node );
	}
}

void FreeBehaviorTree( AIBehaviorTree_t *tree )
{
	if ( tree )
	{
		FreeNode( ( AIGenericNode_t * ) tree->root );

		BG_Free( tree );
	}
	else
	{
		G_Printf( "WARNING: Attempted to free NULL behavior tree\n" );
	}
}
