// Emacs style mode select	 -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//		Movement, collision handling.
//		Shooting and aiming.
//
//-----------------------------------------------------------------------------


#include <stdlib.h>
#include <math.h>

#include "vectors.h"

#include "m_alloc.h"
#include "m_bbox.h"
#include "m_random.h"
#include "i_system.h"

#include "doomdef.h"
#include "p_local.h"

#include "s_sound.h"

// State.
#include "doomstat.h"
#include "r_state.h"

#include "z_zone.h"

fixed_t 		tmbbox[4];
static mobj_t  *tmthing;
static int 		tmflags;
static fixed_t	tmx;
static fixed_t	tmy;
static fixed_t	tmz;	// [RH] Needed for third dimension of teleporters
static int		pe_x;	// Pain Elemental position for Lost Soul checks	// phares
static int		pe_y;	// Pain Elemental position for Lost Soul checks	// phares
static int		ls_x;	// Lost Soul position for Lost Soul checks		// phares
static int		ls_y;	// Lost Soul position for Lost Soul checks		// phares

BOOL oldshootactivation;	// [RH] True if no distinction is made between
							//		projectile cross and projectile hit

// If "floatok" true, move would be ok
// if within "tmfloorz - tmceilingz".
BOOL 			floatok;

fixed_t 		tmfloorz;
fixed_t 		tmceilingz;
fixed_t 		tmdropoffz;

// keep track of the line that lowers the ceiling,
// so missiles don't explode against sky hack walls
line_t* 		ceilingline;

// keep track of special lines as they are hit,
// but don't process them until the move is proven valid
// [RH] MaxSpecialCross	grows as needed
int				MaxSpecialCross = 0;

line_t** 		spechit;
int 			numspechit;

// Temporary holder for thing_sectorlist threads
msecnode_t* sector_list = NULL;		// phares 3/16/98



//
// TELEPORT MOVE
// 

//
// PIT_StompThing
//
static BOOL StompAlwaysFrags;

BOOL PIT_StompThing (mobj_t* thing)
{
	fixed_t blockdist;

	if (!(thing->flags & MF_SHOOTABLE))
		return true;

	// don't clip against self
	if (thing == tmthing)
		return true;

	blockdist = thing->radius + tmthing->radius;
	
	if (abs(thing->x - tmx) >= blockdist || abs(thing->y - tmy) >= blockdist)
	{
		// didn't hit it
		return true;
	}

	// [RH] Z-Check
	if (!olddemo) {
		if (tmz > thing->z + thing->height)
			return true;        // overhead
		if (tmz+tmthing->height < thing->z)
			return true;        // underneath
	}

	// monsters don't stomp things except on boss level
	if (StompAlwaysFrags) {
		P_DamageMobj (thing, tmthing, tmthing, 10000, MOD_TELEFRAG);
		return true;
	}
	return false;
}


//
// P_TeleportMove
//
// [RH] Added telefrag parameter: When true, anything in the spawn spot
//		will always be telefragged, and the move will be successful.
//		Added z parameter. Originally, the thing's z was set *after* the
//		move was made, so the height checking I added for 1.13 could
//		potentially erroneously indicate the move was okay if the thing
//		was being teleported between two non-overlapping height ranges.
BOOL P_TeleportMove (mobj_t *thing, fixed_t x, fixed_t y, fixed_t z, BOOL telefrag)
{
	int 				xl;
	int 				xh;
	int 				yl;
	int 				yh;
	int 				bx;
	int 				by;
	
	subsector_t*		newsubsec;
	
	// kill anything occupying the position
	tmthing = thing;
	tmflags = thing->flags;
		
	tmx = x;
	tmy = y;
	tmz = z;
		
	tmbbox[BOXTOP] = y + tmthing->radius;
	tmbbox[BOXBOTTOM] = y - tmthing->radius;
	tmbbox[BOXRIGHT] = x + tmthing->radius;
	tmbbox[BOXLEFT] = x - tmthing->radius;

	newsubsec = R_PointInSubsector (x,y);
	ceilingline = NULL;
	
	// The base floor/ceiling is from the subsector
	// that contains the point.
	// Any contacted lines the step closer together
	// will adjust them.
	tmfloorz = tmdropoffz = newsubsec->sector->floorheight;
	tmceilingz = newsubsec->sector->ceilingheight;
						
	validcount++;
	numspechit = 0;

	StompAlwaysFrags = tmthing->player || (level.flags & LEVEL_MONSTERSTELEFRAG) ||
						(!olddemo && telefrag);

	// stomp on any things contacted
	xl = (tmbbox[BOXLEFT] - bmaporgx - MAXRADIUS)>>MAPBLOCKSHIFT;
	xh = (tmbbox[BOXRIGHT] - bmaporgx + MAXRADIUS)>>MAPBLOCKSHIFT;
	yl = (tmbbox[BOXBOTTOM] - bmaporgy - MAXRADIUS)>>MAPBLOCKSHIFT;
	yh = (tmbbox[BOXTOP] - bmaporgy + MAXRADIUS)>>MAPBLOCKSHIFT;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			if (!P_BlockThingsIterator(bx,by,PIT_StompThing))
				return false;
	
	// the move is ok,
	// so link the thing into its new position
	P_UnsetThingPosition (thing);

	thing->floorz = tmfloorz;
	thing->ceilingz = tmceilingz;		
	thing->x = x;
	thing->y = y;
	thing->z = z;

	P_SetThingPosition (thing);

	return true;
}

// P_GetMoveFactor() returns the value by which the x,y		// phares 3/19/98
// movements are multiplied to add to player movement.		//     |
															//     V
int P_GetMoveFactor(mobj_t* mo)
{
	int movefactor = ORIG_FRICTION_FACTOR;

	// If the floor is icy or muddy, it's harder to get moving. This is where
	// the different friction factors are applied to 'trying to move'. In
	// p_mobj.c, the friction factors are applied as you coast and slow down.

	int momentum, friction;

	if (!olddemo && boom_friction->value &&
		!(mo->flags & (MF_NOGRAVITY | MF_NOCLIP)))
	{
		friction = mo->friction;
		if (friction == ORIG_FRICTION)				// normal floor
			;
		else if (friction > ORIG_FRICTION)			// ice
		{
			movefactor = mo->movefactor;
			mo->movefactor = ORIG_FRICTION_FACTOR;	// reset
		}
		else										// sludge
		{

			// phares 3/11/98: you start off slowly, then increase as
			// you get better footing

			momentum = (P_AproxDistance (mo->momx, mo->momy));
			movefactor = mo->movefactor;
			if (momentum > MORE_FRICTION_MOMENTUM<<2)
				movefactor <<= 3;

			else if (momentum > MORE_FRICTION_MOMENTUM<<1)
				movefactor <<= 2;

			else if (momentum > MORE_FRICTION_MOMENTUM)
				movefactor <<= 1;

			mo->movefactor = ORIG_FRICTION_FACTOR;	// reset
		}
	}														//     ^
	return (movefactor);									//     |
}															// phares 3/19/98

//
// MOVEMENT ITERATOR FUNCTIONS
//

//																	// phares
// PIT_CrossLine													//   |
// Checks to see if a PE->LS trajectory line crosses a blocking		//   V
// line. Returns false if it does.
//
// tmbbox holds the bounding box of the trajectory. If that box
// does not touch the bounding box of the line in question,
// then the trajectory is not blocked. If the PE is on one side
// of the line and the LS is on the other side, then the
// trajectory is blocked.
//
// Currently this assumes an infinite line, which is not quite
// correct. A more correct solution would be to check for an
// intersection of the trajectory and the line, but that takes
// longer and probably really isn't worth the effort.
//

static // killough 3/26/98: make static
BOOL PIT_CrossLine (line_t* ld)
{
	if (!(ld->flags & ML_TWOSIDED) ||
		(ld->flags & (ML_BLOCKING|ML_BLOCKMONSTERS|ML_BLOCKEVERYTHING)))
		if (!(tmbbox[BOXLEFT]   > ld->bbox[BOXRIGHT]  ||
			  tmbbox[BOXRIGHT]  < ld->bbox[BOXLEFT]   ||
			  tmbbox[BOXTOP]    < ld->bbox[BOXBOTTOM] ||
			  tmbbox[BOXBOTTOM] > ld->bbox[BOXTOP]))
			if (P_PointOnLineSide(pe_x,pe_y,ld) != P_PointOnLineSide(ls_x,ls_y,ld))
				return(false);  // line blocks trajectory				//   ^
	return(true); // line doesn't block trajectory					//   |
}																	// phares

//
// PIT_CheckLine
// Adjusts tmfloorz and tmceilingz as lines are contacted
//

// [RH] Moved this out of PIT_CheckLine()
static void AddSpecialLine (line_t *ld)
{
	if (ld->special)
	{
		// [RH] Grow the spechit array as needed
		if (numspechit >= MaxSpecialCross) {
			MaxSpecialCross = MaxSpecialCross ? MaxSpecialCross * 2 : 8;
			spechit = Realloc (spechit, MaxSpecialCross * sizeof(*spechit));
			DPrintf ("MaxSpecialCross increased to %d\n", MaxSpecialCross);
		}
		spechit[numspechit++] = ld;
	}
}

static // killough 3/26/98: make static
BOOL PIT_CheckLine (line_t *ld)
{
	if (tmbbox[BOXRIGHT] <= ld->bbox[BOXLEFT]
		|| tmbbox[BOXLEFT] >= ld->bbox[BOXRIGHT]
		|| tmbbox[BOXTOP] <= ld->bbox[BOXBOTTOM]
		|| tmbbox[BOXBOTTOM] >= ld->bbox[BOXTOP] )
		return true;

	if (P_BoxOnLineSide (tmbbox, ld) != -1)
		return true;
				
	// A line has been hit
	
	// The moving thing's destination position will cross
	// the given line.
	// If this should not be allowed, return false.
	// If the line is special, keep track of it
	// to process later if the move is proven ok.
	// NOTE: specials are NOT sorted by order,
	// so two special lines that are only 8 pixels apart
	// could be crossed in either order.
	
	if (!ld->backsector) {
		// [RH] Let somebody push it if they want to
		if ((ld->flags & ML_ACTIVATIONMASK) == ML_ACTIVATEPUSH)
			AddSpecialLine (ld);
		return false;			// one sided line
	}
				
	if (!(tmthing->flags & MF_MISSILE) || (ld->flags & ML_BLOCKEVERYTHING))
	{
		if ((ld->flags & (ML_BLOCKING|ML_BLOCKEVERYTHING)) || 	// explicitly blocking everything
			(!tmthing->player && ld->flags & ML_BLOCKMONSTERS)) {	// block monsters only
				// [RH] Add pushable lines to the special list
				if ((ld->flags & ML_ACTIVATIONMASK) == ML_ACTIVATEPUSH)
					AddSpecialLine (ld);
				return false;
		}
	}

	// set openrange, opentop, openbottom
	P_LineOpening (ld); 
		
	// adjust floor / ceiling heights
	if (opentop < tmceilingz)
	{
		tmceilingz = opentop;
		ceilingline = ld;
	}

	if (openbottom > tmfloorz)
		tmfloorz = openbottom;	

	if (lowfloor < tmdropoffz)
		tmdropoffz = lowfloor;
				
	// if contacted a special line, add it to the list
	AddSpecialLine (ld);

	return true;
}

//
// PIT_CheckThing
//
static // killough 3/26/98: make static
BOOL PIT_CheckThing (mobj_t *thing)
{
	fixed_t blockdist;
	BOOL 	solid;
	int 	damage;
				
	// don't clip against self
	if (thing == tmthing)
		return true;
	
	if (!(thing->flags & (MF_SOLID|MF_SPECIAL|MF_SHOOTABLE)) )
		return true;
	
	blockdist = thing->radius + tmthing->radius;

	if (abs(thing->x - tmx) >= blockdist || abs(thing->y - tmy) >= blockdist)
	{
		// didn't hit it
		return true;	
	}
	
	// [RH] Z-Check
	if (!olddemo) {
		if (tmthing->z >= thing->z + thing->height)
			return true;        // overhead (or on)
		if (tmthing->z+tmthing->height < thing->z)
			return true;        // underneath
	}

	// check for skulls slamming into things
	if (tmthing->flags & MF_SKULLFLY)
	{
		damage = ((P_Random(pr_checkthing)%8)+1)*tmthing->info->damage;
		
		P_DamageMobj (thing, tmthing, tmthing, damage, MOD_UNKNOWN);
		
		tmthing->flags &= ~MF_SKULLFLY;
		tmthing->momx = tmthing->momy = tmthing->momz = 0;
		
		P_SetMobjState (tmthing, tmthing->info->spawnstate);
		
		return false;			// stop moving
	}

	
	// missiles can hit other things
	if (tmthing->flags & MF_MISSILE)
	{
		// [RH] Only check here if this is an old demo
		if (olddemo) {
			// see if it went over / under
			if (tmthing->z > thing->z + thing->height)
				return true;				// overhead
			if (tmthing->z+tmthing->height < thing->z)
				return true;				// underneath
		}
				
		if (tmthing->target && (
			tmthing->target->type == thing->type || 
			(tmthing->target->type == MT_KNIGHT && thing->type == MT_BRUISER)||
			(tmthing->target->type == MT_BRUISER && thing->type == MT_KNIGHT) ) )
		{
			// Don't hit same species as originator.
			if (thing == tmthing->target)
				return true;

			// [RH] DeHackEd infighting is here.
			if (!deh.Infight && thing->type != MT_PLAYER)
			{
				// Explode, but do no damage.
				// Let players missile other players.
				return false;
			}
		}
		
		if (! (thing->flags & MF_SHOOTABLE) )
		{
			// didn't do any damage
			return !(thing->flags & MF_SOLID);	
		}
		
		// damage / explode
		damage = ((P_Random(pr_checkthing)%8)+1)*tmthing->info->damage;
		{
			// [RH] figure out the means of death
			int mod;

			switch (tmthing->type) {
				case MT_ROCKET:
					mod = MOD_ROCKET;
					break;
				case MT_PLASMA:
					mod = MOD_PLASMARIFLE;
					break;
				case MT_BFG:
					mod = MOD_BFG_BOOM;
					break;
				default:
					mod = MOD_UNKNOWN;
					break;
			}
			P_DamageMobj (thing, tmthing, tmthing->target, damage, mod);
		}

		// don't traverse any more
		return false;							
	}
	
	// check for special pickup
	if (thing->flags & MF_SPECIAL)
	{
		solid = thing->flags&MF_SOLID;
		if (tmflags&MF_PICKUP)
		{
			// can remove thing
			P_TouchSpecialThing (thing, tmthing);
		}
		return !solid;
	}

	// [RH] Allow step up on top of things up to 24 units (like stairs)
	//		The movement code will automatically place the moving object
	//		on top of the other one.
	if (!olddemo
		&& !(tmthing->z & MF2_JUMPING)
		&& tmthing->z < thing->z + thing->height
		&& thing->z + thing->height - tmthing->z <= 24 * FRACUNIT
		&& thing->z + thing->height + tmthing->height < thing->subsector->sector->ceilingheight
	   ) {
		tmthing->z = thing->z;
		tmthing->momz = 1;
		return true;
	}

	// killough 3/16/98: Allow non-solid moving objects to move through solid
	// ones, by allowing the moving thing (tmthing) to move if it's non-solid,
	// despite another solid thing being in the way.
	// killough 4/11/98: Treat no-clipping things as not blocking

	return !((thing->flags & MF_SOLID && !(thing->flags & MF_NOCLIP))
			 && (tmthing->flags & MF_SOLID || olddemo));

	// return !(thing->flags & MF_SOLID);	// old code -- killough
}

// This routine checks for Lost Souls trying to be spawned		// phares
// across 1-sided lines, impassible lines, or "monsters can't	//   |
// cross" lines. Draw an imaginary line between the PE			//   V
// and the new Lost Soul spawn spot. If that line crosses
// a 'blocking' line, then disallow the spawn. Only search
// lines in the blocks of the blockmap where the bounding box
// of the trajectory line resides. Then check bounding box
// of the trajectory vs. the bounding box of each blocking
// line to see if the trajectory and the blocking line cross.
// Then check the PE and LS to see if they're on different
// sides of the blocking line. If so, return true, otherwise
// false.

BOOL Check_Sides(mobj_t* actor, int x, int y)
{
	int bx,by,xl,xh,yl,yh;

	pe_x = actor->x;
	pe_y = actor->y;
	ls_x = x;
	ls_y = y;

	// Here is the bounding box of the trajectory

	tmbbox[BOXLEFT]   = pe_x < x ? pe_x : x;
	tmbbox[BOXRIGHT]  = pe_x > x ? pe_x : x;
	tmbbox[BOXTOP]    = pe_y > y ? pe_y : y;
	tmbbox[BOXBOTTOM] = pe_y < y ? pe_y : y;

	// Determine which blocks to look in for blocking lines

	xl = (tmbbox[BOXLEFT]   - bmaporgx)>>MAPBLOCKSHIFT;
	xh = (tmbbox[BOXRIGHT]  - bmaporgx)>>MAPBLOCKSHIFT;
	yl = (tmbbox[BOXBOTTOM] - bmaporgy)>>MAPBLOCKSHIFT;
	yh = (tmbbox[BOXTOP]    - bmaporgy)>>MAPBLOCKSHIFT;

	// xl->xh, yl->yh determine the mapblock set to search

	validcount++; // prevents checking same line twice
	for (bx = xl ; bx <= xh ; bx++)
		for (by = yl ; by <= yh ; by++)
		if (!P_BlockLinesIterator(bx,by,PIT_CrossLine))
			return true;										//   ^
	return(false);												//   |
}																// phares

//
// MOVEMENT CLIPPING
//

//
// P_CheckPosition
// This is purely informative, nothing is modified
// (except things picked up).
// 
// in:
//	a mobj_t (can be valid or invalid)
//	a position to be checked
//	 (doesn't need to be related to the mobj_t->x,y)
//
// during:
//	special things are touched if MF_PICKUP
//	early out on solid lines?
//
// out:
//	newsubsec
//	floorz
//	ceilingz
//	tmdropoffz
//	 the lowest point contacted
//	 (monsters won't move to a dropoff)
//	speciallines[]
//	numspeciallines
//
BOOL P_CheckPosition (mobj_t *thing, fixed_t x, fixed_t y)
{
	int xl, xh;
	int yl, yh;
	int bx, by;
	subsector_t *newsubsec;

	tmthing = thing;
	tmflags = thing->flags;
		
	tmx = x;
	tmy = y;
		
	tmbbox[BOXTOP] = y + tmthing->radius;
	tmbbox[BOXBOTTOM] = y - tmthing->radius;
	tmbbox[BOXRIGHT] = x + tmthing->radius;
	tmbbox[BOXLEFT] = x - tmthing->radius;

	newsubsec = R_PointInSubsector (x,y);
	ceilingline = NULL;
	
	// The base floor / ceiling is from the subsector
	// that contains the point.
	// Any contacted lines the step closer together
	// will adjust them.
	tmfloorz = tmdropoffz = newsubsec->sector->floorheight;
	tmceilingz = newsubsec->sector->ceilingheight;
						
	validcount++;
	numspechit = 0;

	if ( tmflags & MF_NOCLIP )
		return true;
	
	// Check things first, possibly picking things up.
	// The bounding box is extended by MAXRADIUS
	// because mobj_ts are grouped into mapblocks
	// based on their origin point, and can overlap
	// into adjacent blocks by up to MAXRADIUS units.
	xl = (tmbbox[BOXLEFT] - bmaporgx - MAXRADIUS)>>MAPBLOCKSHIFT;
	xh = (tmbbox[BOXRIGHT] - bmaporgx + MAXRADIUS)>>MAPBLOCKSHIFT;
	yl = (tmbbox[BOXBOTTOM] - bmaporgy - MAXRADIUS)>>MAPBLOCKSHIFT;
	yh = (tmbbox[BOXTOP] - bmaporgy + MAXRADIUS)>>MAPBLOCKSHIFT;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			if (!P_BlockThingsIterator(bx,by,PIT_CheckThing))
				return false;
	
	// check lines
	xl = (tmbbox[BOXLEFT] - bmaporgx)>>MAPBLOCKSHIFT;
	xh = (tmbbox[BOXRIGHT] - bmaporgx)>>MAPBLOCKSHIFT;
	yl = (tmbbox[BOXBOTTOM] - bmaporgy)>>MAPBLOCKSHIFT;
	yh = (tmbbox[BOXTOP] - bmaporgy)>>MAPBLOCKSHIFT;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			if (!P_BlockLinesIterator (bx,by,PIT_CheckLine))
				return false;

	return true;
}


//
// P_TryMove
// Attempt to move to a new position,
// crossing special lines unless MF_TELEPORT is set.
//
BOOL P_TryMove (mobj_t *thing, fixed_t x, fixed_t y,
				BOOL dropoff) // killough 3/15/98: allow dropoff as option
{
	fixed_t 	oldx;
	fixed_t 	oldy;
	int 		side;
	int 		oldside;
	line_t* 	ld;

	floatok = false;
	if (!P_CheckPosition (thing, x, y))
		return false;			// solid wall or thing
	
	if ( !(thing->flags & MF_NOCLIP) )
	{
#ifdef CEILCARRY
		// killough 4/11/98: if the thing hangs from
		// a ceiling, don't worry about it fitting

		if (!(thing->flags & MF_SPAWNCEILING) || demo_compatibility)
#endif

		if (tmceilingz - tmfloorz < thing->height)
			return false;		// doesn't fit

		floatok = true;
		
		if ( !(thing->flags&MF_TELEPORT) && tmceilingz - thing->z < thing->height)
			return false;		// mobj must lower itself to fit

		if ( !(thing->flags&MF_TELEPORT) && tmfloorz - thing->z > 24*FRACUNIT )
			return false;		// too big a step up

		// killough 3/15/98: Allow certain objects to drop off

		if (olddemo || !dropoff)
			if ( !(thing->flags&(MF_DROPOFF|MF_FLOAT))
				&& tmfloorz - tmdropoffz > 24*FRACUNIT )
				return false;	// don't stand over a dropoff
	}
	
	// the move is ok,
	// so link the thing into its new position
	P_UnsetThingPosition (thing);

	oldx = thing->x;
	oldy = thing->y;
	thing->floorz = tmfloorz;
	thing->ceilingz = tmceilingz;
	thing->x = x;
	thing->y = y;

	P_SetThingPosition (thing);
	
	// if any special lines were hit, do the effect
	if (! (thing->flags&(MF_TELEPORT|MF_NOCLIP)) )
	{
		while (numspechit--)
		{
			// see if the line was crossed
			ld = spechit[numspechit];
			side = P_PointOnLineSide (thing->x, thing->y, ld);
			oldside = P_PointOnLineSide (oldx, oldy, ld);
			if (side != oldside && ld->special)
				P_CrossSpecialLine (ld-lines, oldside, thing);
		}
	}

	return true;
}


//
// P_ThingHeightClip
// Takes a valid thing and adjusts the thing->floorz,
// thing->ceilingz, and possibly thing->z.
// This is called for all nearby monsters
// whenever a sector changes height.
// If the thing doesn't fit,
// the z will be set to the lowest value
// and false will be returned.
//
BOOL P_ThingHeightClip (mobj_t* thing)
{
	// [RH] Move mobjs standing on other mobjs
	mobj_t		*floor;
	fixed_t		oldfloorz;

	oldfloorz = thing->floorz;
	floor = P_FindFloor (thing);

	P_CheckPosition (thing, thing->x, thing->y);		
	// what about stranding a monster partially off an edge?
		
	thing->floorz = tmfloorz;
	thing->ceilingz = tmceilingz;

	if (floor)
	{
		// walking monsters rise and fall with the floor
		if (floor == (mobj_t *)-1)
			thing->z = thing->floorz;
		else
			thing->z -= oldfloorz - tmfloorz;
	}
	// don't adjust a floating monster unless forced to
	else if (thing->z + thing->height > thing->ceilingz)
		thing->z = thing->ceilingz - thing->height;

	if (thing->ceilingz - thing->floorz < thing->height)
		return false;
				
	return true;
}



//
// SLIDE MOVE
// Allows the player to slide along any angled walls.
//
fixed_t 		bestslidefrac;
fixed_t 		secondslidefrac;

line_t* 		bestslideline;
line_t* 		secondslideline;

mobj_t* 		slidemo;

fixed_t 		tmxmove;
fixed_t 		tmymove;

extern mobj_t *onground;


//
// P_HitSlideLine
// Adjusts the xmove / ymove
// so that the next move will slide along the wall.
// If the floor is icy, then you can bounce off a wall.				// phares
//
void P_HitSlideLine (line_t* ld)
{
	int 	side;

	angle_t lineangle;
	angle_t moveangle;
	angle_t deltaangle;
	
	fixed_t movelen;
	fixed_t newlen;
	BOOL	icyfloor;	// is floor icy?							// phares
																	//   |
	// Under icy conditions, if the angle of approach to the wall	//   V
	// is more than 45 degrees, then you'll bounce and lose half
	// your momentum. If less than 45 degrees, you'll slide along
	// the wall. 45 is arbitrary and is believable.

	// Check for the special cases of horz or vert walls.

	if (!olddemo && boom_friction->value && slidemo->player)
		// player must be on ground
		icyfloor = ((onground == (mobj_t *)-1) && (slidemo->friction > ORIG_FRICTION));
	else
		icyfloor = false;

	if (ld->slopetype == ST_HORIZONTAL)
	{
		if (icyfloor && (abs(tmymove) > abs(tmxmove)))
		{
			tmxmove /= 2; // absorb half the momentum
			tmymove = -tmymove/2;
			S_StartSound (slidemo, "*grunt1", 78); // oooff!
		}
		else
			tmymove = 0; // no more movement in the Y direction
		return;
	}

	if (ld->slopetype == ST_VERTICAL)
	{
		if (icyfloor && (abs(tmxmove) > abs(tmymove)))
		{
			tmxmove = -tmxmove/2; // absorb half the momentum
			tmymove /= 2;
			S_StartSound (slidemo, "*grunt1", 78); // oooff!	//   ^
		}																//   |
		else															// phares
			tmxmove = 0; // no more movement in the X direction
		return;
	}

	// The wall is angled. Bounce if the angle of approach is		// phares
	// less than 45 degrees.										// phares

	side = P_PointOnLineSide (slidemo->x, slidemo->y, ld);

	lineangle = R_PointToAngle2 (0,0, ld->dx, ld->dy);

	if (side == 1)
		lineangle += ANG180;

	moveangle = R_PointToAngle2 (0,0, tmxmove, tmymove);

	// killough 3/2/98:
	// The moveangle+=10 breaks v1.9 demo compatibility in
	// some demos, so it needs demo_compatibility switch.

	if (!olddemo)
		moveangle += 10;	// prevents sudden path reversal due to	// phares
							// rounding error						//   |
	deltaangle = moveangle-lineangle;								//   V
	movelen = P_AproxDistance (tmxmove, tmymove);
	if (icyfloor && (deltaangle > ANG45) && (deltaangle < ANG90+ANG45))
	{
		moveangle = lineangle - deltaangle;
		movelen /= 2; // absorb
		S_StartSound (slidemo, "*grunt1", 78); // oooff!
		moveangle >>= ANGLETOFINESHIFT;
		tmxmove = FixedMul (movelen, finecosine[moveangle]);
		tmymove = FixedMul (movelen, finesine[moveangle]);
	}																//   ^
	else															//   |
	{																// phares
		if (deltaangle > ANG180)
			deltaangle += ANG180;
		//	I_Error ("SlideLine: ang>ANG180");

		lineangle >>= ANGLETOFINESHIFT;
		deltaangle >>= ANGLETOFINESHIFT;

		movelen = P_AproxDistance (tmxmove, tmymove);
		newlen = FixedMul (movelen, finecosine[deltaangle]);

		tmxmove = FixedMul (newlen, finecosine[lineangle]);
		tmymove = FixedMul (newlen, finesine[lineangle]);
	}																// phares
}


//
// PTR_SlideTraverse
//
BOOL PTR_SlideTraverse (intercept_t* in)
{
	line_t* 	li;
		
	if (!in->isaline)
		I_Error ("PTR_SlideTraverse: not a line?");
				
	li = in->d.line;
	
	if ( ! (li->flags & ML_TWOSIDED) )
	{
		if (P_PointOnLineSide (slidemo->x, slidemo->y, li))
		{
			// don't hit the back side
			return true;				
		}
		goto isblocking;
	}

	// set openrange, opentop, openbottom
	P_LineOpening (li);
	
	if (openrange < slidemo->height)
		goto isblocking;				// doesn't fit
				
	if (opentop - slidemo->z < slidemo->height)
		goto isblocking;				// mobj is too high

	if (openbottom - slidemo->z > 24*FRACUNIT )
		goto isblocking;				// too big a step up

	// this line doesn't block movement
	return true;				
		
	// the line does block movement,
	// see if it is closer than best so far
  isblocking:			
	if (in->frac < bestslidefrac)
	{
		secondslidefrac = bestslidefrac;
		secondslideline = bestslideline;
		bestslidefrac = in->frac;
		bestslideline = li;
	}
		
	return false;		// stop
}



//
// P_SlideMove
// The momx / momy move is bad, so try to slide
// along a wall.
// Find the first line hit, move flush to it,
// and slide along it
//
// This is a kludgy mess.
//
void P_SlideMove (mobj_t* mo)
{
	fixed_t 			leadx;
	fixed_t 			leady;
	fixed_t 			trailx;
	fixed_t 			traily;
	fixed_t 			newx;
	fixed_t 			newy;
	int 				hitcount;
				
	slidemo = mo;
	hitcount = 0;
	
  retry:
	if (++hitcount == 3)
		goto stairstep; 		// don't loop forever

	
	// trace along the three leading corners
	if (mo->momx > 0)
	{
		leadx = mo->x + mo->radius;
		trailx = mo->x - mo->radius;
	}
	else
	{
		leadx = mo->x - mo->radius;
		trailx = mo->x + mo->radius;
	}
		
	if (mo->momy > 0)
	{
		leady = mo->y + mo->radius;
		traily = mo->y - mo->radius;
	}
	else
	{
		leady = mo->y - mo->radius;
		traily = mo->y + mo->radius;
	}
				
	bestslidefrac = FRACUNIT+1;
		
	P_PathTraverse ( leadx, leady, leadx+mo->momx, leady+mo->momy,
					 PT_ADDLINES, PTR_SlideTraverse );
	P_PathTraverse ( trailx, leady, trailx+mo->momx, leady+mo->momy,
					 PT_ADDLINES, PTR_SlideTraverse );
	P_PathTraverse ( leadx, traily, leadx+mo->momx, traily+mo->momy,
					 PT_ADDLINES, PTR_SlideTraverse );
	
	// move up to the wall
	if (bestslidefrac == FRACUNIT+1)
	{
		// the move most have hit the middle, so stairstep
	  stairstep:
		// killough 3/15/98: Allow objects to drop off ledges

		if (!P_TryMove (mo, mo->x, mo->y + mo->momy, true))

			// phares 5/4/98: kill momentum if you can't move at all
			// This eliminates player bobbing if pressed against a wall
			// while on ice.

			if (!P_TryMove (mo, mo->x + mo->momx, mo->y, true))
				if (!olddemo)
					mo->momx = mo->momy = 0;
		return;
	}

	// fudge a bit to make sure it doesn't hit
	bestslidefrac -= 0x800; 	
	if (bestslidefrac > 0)
	{
		newx = FixedMul (mo->momx, bestslidefrac);
		newy = FixedMul (mo->momy, bestslidefrac);
		
		// killough 3/15/98: Allow objects to drop off ledges

		if (!P_TryMove (mo, mo->x+newx, mo->y+newy, true))
			goto stairstep;
	}
	
	// Now continue along the wall.
	// First calculate remainder.
	bestslidefrac = FRACUNIT-(bestslidefrac+0x800);
	
	if (bestslidefrac > FRACUNIT)
		bestslidefrac = FRACUNIT;
	
	if (bestslidefrac <= 0)
		return;
	
	tmxmove = FixedMul (mo->momx, bestslidefrac);
	tmymove = FixedMul (mo->momy, bestslidefrac);

	P_HitSlideLine (bestslideline); 	// clip the moves

	mo->momx = tmxmove;
	mo->momy = tmymove;
				
    // killough 3/15/98: Allow objects to drop off ledges

	if (!P_TryMove (mo, mo->x+tmxmove, mo->y+tmymove, true))
		goto retry;
}


//
// P_LineAttack
//
mobj_t* 		linetarget; 	// who got hit (or NULL)
mobj_t* 		shootthing;

// Height if not aiming up or down
// ???: use slope for monsters?
fixed_t 		shootz; 

int 			la_damage;
fixed_t 		attackrange;

fixed_t 		aimslope;

// slopes to top and bottom of target
// killough 4/20/98: make static instead of using ones in p_sight.c

static fixed_t	topslope;
static fixed_t	bottomslope;


//
// PTR_AimTraverse
// Sets linetaget and aimslope when a target is aimed at.
//
BOOL PTR_AimTraverse (intercept_t* in)
{
	line_t* 			li;
	mobj_t* 			th;
	fixed_t 			slope;
	fixed_t 			thingtopslope;
	fixed_t 			thingbottomslope;
	fixed_t 			dist;
				
	if (in->isaline)
	{
		li = in->d.line;
		
		if ( !(li->flags & ML_TWOSIDED) )
			return false;				// stop
		
		// Crosses a two sided line.
		// A two sided line will restrict
		// the possible target ranges.
		P_LineOpening (li);
		
		if (openbottom >= opentop)
			return false;				// stop
		
		dist = FixedMul (attackrange, in->frac);

		if (li->frontsector->floorheight != li->backsector->floorheight)
		{
			slope = FixedDiv (openbottom - shootz , dist);
			if (slope > bottomslope)
				bottomslope = slope;
		}
				
		if (li->frontsector->ceilingheight != li->backsector->ceilingheight)
		{
			slope = FixedDiv (opentop - shootz , dist);
			if (slope < topslope)
				topslope = slope;
		}
				
		if (topslope <= bottomslope)
			return false;				// stop
						
		return true;					// shot continues
	}
	
	// shoot a thing
	th = in->d.thing;
	if (th == shootthing)
		return true;					// can't shoot self
	
	if (!(th->flags&MF_SHOOTABLE))
		return true;					// corpse or something

	// check angles to see if the thing can be aimed at
	dist = FixedMul (attackrange, in->frac);
	thingtopslope = FixedDiv (th->z+th->height - shootz , dist);

	if (thingtopslope < bottomslope)
		return true;					// shot over the thing

	thingbottomslope = FixedDiv (th->z - shootz, dist);

	if (thingbottomslope > topslope)
		return true;					// shot under the thing
	
	// this thing can be hit!
	if (thingtopslope > topslope)
		thingtopslope = topslope;
	
	if (thingbottomslope < bottomslope)
		thingbottomslope = bottomslope;

	aimslope = (thingtopslope+thingbottomslope)/2;
	linetarget = th;

	return false;						// don't go any farther
}


//
// PTR_ShootTraverse
//
BOOL PTR_ShootTraverse (intercept_t* in)
{
	fixed_t 			x;
	fixed_t 			y;
	fixed_t 			z;
	fixed_t 			frac;
	
	line_t* 			li;
	
	mobj_t* 			th;

	fixed_t 			slope;
	fixed_t 			dist;
	fixed_t 			thingtopslope;
	fixed_t 			thingbottomslope;
				
	if (in->isaline)
	{
		li = in->d.line;
		
		if (oldshootactivation && li->special)
			P_ShootSpecialLine (shootthing, li);

		if ( !(li->flags & ML_TWOSIDED) || (li->flags & ML_BLOCKEVERYTHING))
			goto hitline;
		
		// crosses a two sided line
		P_LineOpening (li);
				
		dist = FixedMul (attackrange, in->frac);

		if (li->frontsector->floorheight != li->backsector->floorheight)
		{
			slope = FixedDiv (openbottom - shootz , dist);
			if (slope > aimslope)
				goto hitline;
		}
				
		if (li->frontsector->ceilingheight != li->backsector->ceilingheight)
		{
			slope = FixedDiv (opentop - shootz, dist);
			if (slope < aimslope)
				goto hitline;
		}

		// shot continues
		// [RH] Possibly trigger activation
		if (!oldshootactivation && li->special)
			P_ShotCrossSpecialLine (shootthing, li);

		return true;
		
		
		// hit line
	  hitline:
		// [RH] Possibly trigger activation
		if (!oldshootactivation && li->special)
			P_ShootSpecialLine (shootthing, li);

		// position a bit closer
		frac = in->frac - FixedDiv (4*FRACUNIT,attackrange);
		z = shootz + FixedMul (aimslope, FixedMul(frac, attackrange));

		if (li->frontsector->ceilingpic == skyflatnum)
		{
			// don't shoot the sky!
			if (z > li->frontsector->ceilingheight)
				return false;
			
			// it's a sky hack wall
			if	(li->backsector && li->backsector->ceilingpic == skyflatnum)
				// fix bullet-eaters -- killough:
				// WARNING: Almost all demos will lose sync without this
				// demo_compatibility flag check!!! killough 1/18/98
				if (olddemo || li->backsector->ceilingheight < z)
					return false;			
		}

		// [RH] If the trace went below/above the floor/ceiling, make the puff
		//		appear in the right place and not on a wall.
		{
			fixed_t ceilingheight, floorheight;

			if (!li->backsector || !P_PointOnLineSide (trace.x, trace.y, li)) {
				ceilingheight = li->frontsector->ceilingheight;
				floorheight = li->frontsector->floorheight;
			} else {
				ceilingheight = li->backsector->ceilingheight;
				floorheight = li->backsector->floorheight;
			}

			if (z < floorheight) {
				frac = FixedDiv (FixedMul (floorheight - shootz, frac), z - shootz);
				z = floorheight;
			} else if (z > ceilingheight) {
				// Puffs on the ceiling need to be lowered to compensate for the height of the puff
				ceilingheight -= mobjinfo[MT_PUFF].height;
				frac = FixedDiv (FixedMul (ceilingheight - shootz, frac), z - shootz);
				z = ceilingheight;
			}
		}

		x = trace.x + FixedMul (trace.dx, frac);
		y = trace.y + FixedMul (trace.dy, frac);

		// Spawn bullet puffs.
		P_SpawnPuff (x, y, z);
		
		// don't go any farther
		return false;	
	}
	
	// shoot a thing
	th = in->d.thing;
	if (th == shootthing)
		return true;			// can't shoot self
	
	if (!(th->flags&MF_SHOOTABLE))
		return true;			// corpse or something
				
	// check angles to see if the thing can be aimed at
	dist = FixedMul (attackrange, in->frac);
	thingtopslope = FixedDiv (th->z+th->height - shootz , dist);

	if (thingtopslope < aimslope)
		return true;			// shot over the thing

	thingbottomslope = FixedDiv (th->z - shootz, dist);

	if (thingbottomslope > aimslope)
		return true;			// shot under the thing

	
	// hit thing
	// position a bit closer
	frac = in->frac - FixedDiv (10*FRACUNIT,attackrange);

	x = trace.x + FixedMul (trace.dx, frac);
	y = trace.y + FixedMul (trace.dy, frac);
	z = shootz + FixedMul (aimslope, FixedMul(frac, attackrange));

	// Spawn bullet puffs or blod spots,
	// depending on target type.
	if ((in->d.thing->flags & MF_NOBLOOD) || (in->d.thing->flags2 & MF2_INVULNERABLE))
		P_SpawnPuff (x,y,z);
	else
		P_SpawnBlood (x,y,z, la_damage);

	if (la_damage) {
		// [RH] try and figure out means of death;
		int mod = MOD_UNKNOWN;

		if (shootthing->player) {
			switch (shootthing->player->readyweapon) {
				case wp_fist:
					mod = MOD_FIST;
					break;
				case wp_pistol:
					mod = MOD_PISTOL;
					break;
				case wp_shotgun:
					mod = MOD_SHOTGUN;
					break;
				case wp_chaingun:
					mod = MOD_CHAINGUN;
					break;
				case wp_supershotgun:
					mod = MOD_SSHOTGUN;
					break;
				default:
					break;
			}
		}

		P_DamageMobj (th, shootthing, shootthing, la_damage, mod);
	}

	// don't go any farther
	return false;
		
}


//
// P_AimLineAttack
//
fixed_t P_AimLineAttack (mobj_t *t1, angle_t angle, fixed_t distance)
{
	fixed_t 	x2;
	fixed_t 	y2;
		
	angle >>= ANGLETOFINESHIFT;
	shootthing = t1;
	
	x2 = t1->x + (distance>>FRACBITS)*finecosine[angle];
	y2 = t1->y + (distance>>FRACBITS)*finesine[angle];
	shootz = t1->z + (t1->height>>1) + 8*FRACUNIT;

	// can't shoot outside view angles
	// [RH] also adjust for view pitch
	{
		fixed_t slopediff = FixedMul (t1->pitch, -40960);
		topslope = 100*FRACUNIT/160 + slopediff;
		bottomslope = -100*FRACUNIT/160 + slopediff;
	}
	
	attackrange = distance;
	linetarget = NULL;
		
	P_PathTraverse ( t1->x, t1->y,
					 x2, y2,
					 PT_ADDLINES|PT_ADDTHINGS,
					 PTR_AimTraverse );
				
	if (linetarget)
		return aimslope;

	return 0;
}
 

//
// P_LineAttack
// If damage == 0, it is just a test trace
// that will leave linetarget set.
//
void P_LineAttack (mobj_t *t1, angle_t angle, fixed_t distance,
				   fixed_t slope, int damage)
{
	fixed_t 	x2;
	fixed_t 	y2;
		
	angle >>= ANGLETOFINESHIFT;
	shootthing = t1;
	la_damage = damage;
	x2 = t1->x + (distance>>FRACBITS)*finecosine[angle];
	y2 = t1->y + (distance>>FRACBITS)*finesine[angle];
	shootz = t1->z + (t1->height>>1) + 8*FRACUNIT;
	attackrange = distance;
	aimslope = slope;
				
	P_PathTraverse (t1->x, t1->y, x2, y2, PT_ADDLINES|PT_ADDTHINGS, PTR_ShootTraverse);
}
 


//
// USE LINES
//
mobj_t *usething;

BOOL PTR_UseTraverse (intercept_t *in)
{
	int side;
		
	if (!in->d.line->special)
	{
		P_LineOpening (in->d.line);
		if (openrange <= 0)
		{
			S_StartSound (usething, "*grunt1", 78);
			
			// can't use through a wall
			return false;		
		}
		// not a special line, but keep checking
		return true ;			
	}
		
	side = 0;
	if (P_PointOnLineSide (usething->x, usething->y, in->d.line) == 1)
		side = 1;
	
	//	return false;			// don't use back side
		
	P_UseSpecialLine (usething, in->d.line, side);

	//WAS can't use for than one special line in a row
	//jff 3/21/98 NOW multiple use allowed with enabling line flag

	return (!olddemo && (in->d.line->flags & ML_PASSUSE)) ? true : false;
}

// Returns false if a "oof" sound should be made because of a blocking
// linedef. Makes 2s middles which are impassable, as well as 2s uppers
// and lowers which block the player, cause the sound effect when the
// player tries to activate them. Specials are excluded, although it is
// assumed that all special linedefs within reach have been considered
// and rejected already (see P_UseLines).
//
// by Lee Killough
//

BOOL PTR_NoWayTraverse(intercept_t* in)
{
	line_t *ld = in->d.line;					// This linedef

	return ld->special || !(					// Ignore specials
		ld->flags & (ML_BLOCKING|ML_BLOCKEVERYTHING) || (			// Always blocking
		P_LineOpening(ld),						// Find openings
		openrange <= 0 ||						// No opening
		openbottom > usething->z+24*FRACUNIT ||	// Too high it blocks
		opentop < usething->z+usething->height	// Too low it blocks
	)
	);
}

//
// P_UseLines
// Looks for special lines in front of the player to activate.
//
void P_UseLines (player_t*		player) 
{
	int 		angle;
	fixed_t 	x1;
	fixed_t 	y1;
	fixed_t 	x2;
	fixed_t 	y2;
		
	usething = player->mo;
				
	angle = player->mo->angle >> ANGLETOFINESHIFT;

	x1 = player->mo->x;
	y1 = player->mo->y;
	x2 = x1 + (USERANGE>>FRACBITS)*finecosine[angle];
	y2 = y1 + (USERANGE>>FRACBITS)*finesine[angle];

	// old code:
	//
	// P_PathTraverse ( x1, y1, x2, y2, PT_ADDLINES, PTR_UseTraverse );
	//
	// This added test makes the "oof" sound work on 2s lines -- killough:

	if (P_PathTraverse ( x1, y1, x2, y2, PT_ADDLINES, PTR_UseTraverse ))
		if (!P_PathTraverse ( x1, y1, x2, y2, PT_ADDLINES, PTR_NoWayTraverse ))
			S_StartSound (usething, "*grunt1", 78);
}


// -> [RH] Z-Check
static mobj_t *cfthing, *sfthing;

BOOL PIT_CheckFloor (mobj_t *thing)
{
	fixed_t blockdist;

	// don't stand on self
	if (thing == cfthing)
		return true;

	// don't stand on nonsolid objects
	if (!(thing->flags & MF_SOLID))
		return true;

	// don't stand on dead corpses
	if (thing->flags & MF_CORPSE && thing->health <= 0)
		return true;

	blockdist = thing->radius + cfthing->radius;
	
	if ( abs(thing->x - cfthing->x) >= blockdist
		 || abs(thing->y - cfthing->y) >= blockdist )
		// nowhere near it
		return true;

	if (cfthing->z > thing->z + thing->height ||	// above it
		cfthing->z < thing->z)						// below it
		return true;
	
	// on (in) it
	sfthing = thing;
	return false;
}

mobj_t *P_FindFloor (mobj_t *thing)
{
	int 		xl;
	int 		xh;
	int 		yl;
	int 		yh;
	
	int 		bx;
	int 		by;

	fixed_t		ffbox[4];

	if (thing->z <= thing->floorz)
		return (mobj_t *)-1;		// On floor

	if (!(thing->flags & MF_SOLID))	// Non-solid objects can't stand on other mobjs
		return NULL;

	if (thing->flags & (MF_NOCLIP|MF_NOBLOCKMAP))
		return NULL;				// Noclip objects don't use mobjs as floors

	if (thing->flags & MF_CORPSE && thing->health <= 0)
		return NULL;				// Dead corpses also don't sense other mobjs

	ffbox[BOXTOP] = thing->y + thing->radius;
	ffbox[BOXBOTTOM] = thing->y - thing->radius;
	ffbox[BOXRIGHT] = thing->x + thing->radius;
	ffbox[BOXLEFT] = thing->x - thing->radius;

	// check for things underneath us
	xl = (ffbox[BOXLEFT] - bmaporgx - MAXRADIUS)>>MAPBLOCKSHIFT;
	xh = (ffbox[BOXRIGHT] - bmaporgx + MAXRADIUS)>>MAPBLOCKSHIFT;
	yl = (ffbox[BOXBOTTOM] - bmaporgy - MAXRADIUS)>>MAPBLOCKSHIFT;
	yh = (ffbox[BOXTOP] - bmaporgy + MAXRADIUS)>>MAPBLOCKSHIFT;

	cfthing = thing;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			if (!P_BlockThingsIterator(bx,by,PIT_CheckFloor))
				return sfthing;

	return NULL;				// In midair
}

BOOL PIT_CheckCeiling (mobj_t *thing)
{
	fixed_t blockdist;

	// don't hit self
	if (thing == cfthing)
		return true;

	// don't hit nonsolid objects
	if (!(thing->flags & MF_SOLID))
		return true;

	// don't hit dead corpses
	if (thing->flags & MF_CORPSE && thing->health <= 0)
		return true;

	blockdist = thing->radius + cfthing->radius;
	
	if ( abs(thing->x - cfthing->x) >= blockdist
		 || abs(thing->y - cfthing->y) >= blockdist )
		// nowhere near it
		return true;

	if (cfthing->z + cfthing->height < thing->z ||					// below it
		cfthing->z + cfthing->height > thing->z + thing->height)	// above it
		return true;
	
	// hit it
	sfthing = thing;
	return false;
}

mobj_t *P_FindCeiling (mobj_t *thing)
{
	int 		xl;
	int 		xh;
	int 		yl;
	int 		yh;
	
	int 		bx;
	int 		by;

	fixed_t		ffbox[4];

	if (thing->z + thing->height >= thing->ceilingz)
		return (mobj_t *)-1;		// Hit ceiling

	if (!(thing->flags & MF_SOLID))	// Non-solid objects aren't blocked by other mobjs
		return NULL;

	if (thing->flags & (MF_NOCLIP|MF_NOBLOCKMAP))
		return NULL;				// Noclip objects don't use mobjs as ceilings

	if (thing->flags & MF_CORPSE && thing->health <= 0)
		return NULL;				// Dead corpses also don't sense other mobjs

	ffbox[BOXTOP] = thing->y + thing->radius;
	ffbox[BOXBOTTOM] = thing->y - thing->radius;
	ffbox[BOXRIGHT] = thing->x + thing->radius;
	ffbox[BOXLEFT] = thing->x - thing->radius;

	// check for things above us
	xl = (ffbox[BOXLEFT] - bmaporgx - MAXRADIUS)>>MAPBLOCKSHIFT;
	xh = (ffbox[BOXRIGHT] - bmaporgx + MAXRADIUS)>>MAPBLOCKSHIFT;
	yl = (ffbox[BOXBOTTOM] - bmaporgy - MAXRADIUS)>>MAPBLOCKSHIFT;
	yh = (ffbox[BOXTOP] - bmaporgy + MAXRADIUS)>>MAPBLOCKSHIFT;

	cfthing = thing;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			if (!P_BlockThingsIterator(bx,by,PIT_CheckCeiling))
				return sfthing;

	return NULL;				// In midair
}

void P_StandOnThing (mobj_t *thing, mobj_t *spot)
{
	if (!spot)
		spot = P_FindFloor (thing);

	if (spot && spot != (mobj_t *)-1)
		thing->z = spot->z + spot->height;
	else
		thing->z = thing->floorz;

	if (thing->flags2 & MF2_JUMPING)
	thing->flags2 &= ~MF2_JUMPING;
}
// [RH] Z-Check <-


//
// RADIUS ATTACK
//
mobj_t* 		bombsource;
mobj_t* 		bombspot;
int 			bombdamage;
float			bombdamagefloat;
int				bombmod;
vec3_t			bombvec;


//
// PIT_RadiusAttack
// "bombsource" is the creature
// that caused the explosion at "bombspot".
// [RH] Now it knows about vertical distances and
//      can thrust things vertically, too.
//

// [RH] Damage scale to apply to thing that shot the missile.
cvar_t *splashfactor;
static float selfthrustscale;

void SplashFactorCallback (cvar_t *var) {
	if (var->value <= 0.0f) {
		SetCVarFloat (var, 1.0f);
	} else {
		selfthrustscale = 1.0f / var->value;
	}
}

BOOL PIT_RadiusAttack (mobj_t *thing)
{
	if (!(thing->flags & MF_SHOOTABLE) )
		return true;

	// Boss spider and cyborg
	// take no damage from concussion.
	if (thing->type == MT_CYBORG || thing->type == MT_SPIDER)
		return true;	

	// Barrels always use the original code, since this makes
	// them far too "active."
	if (!olddemo && bombspot->type != MT_BARREL && thing->type != MT_BARREL) {
		// [RH] New code (based on stuff in Q2)
		float points;
		vec3_t thingvec;

		VectorPosition (thing, thingvec);
		thingvec[2] += (float)(thing->height >> (FRACBITS+1));
		{
			vec3_t v;
			float len;

			VectorSubtract (bombvec, thingvec, v);
			len = VectorLength (v);
			points = bombdamagefloat - len;
		}
		if (thing == bombsource)
			points = points * splashfactor->value;
		if (points > 0) {
			if (P_CheckSight (thing, bombspot, true)) {
				vec3_t dir;
				float thrust;
				fixed_t momx = thing->momx;
				fixed_t momy = thing->momy;

				P_DamageMobj (thing, bombspot, bombsource, (int)points, bombmod);
				
				thrust = points * 35000.0f / (float)thing->info->mass;
				VectorSubtract (thingvec, bombvec, dir);
				VectorScale (dir, thrust, dir);
				if (bombsource != thing) {
					dir[2] *= 0.5f;
				} else if (splashfactor->value) {
					dir[0] *= selfthrustscale;
					dir[1] *= selfthrustscale;
					dir[2] *= selfthrustscale;
				}
				thing->momx = momx + (fixed_t)(dir[0]);
				thing->momy = momy + (fixed_t)(dir[1]);
				thing->momz += (fixed_t)(dir[2]);
			}
		}
	} else {
		// [RH] Old code in a futile attempt to maintain demo compatibility
		fixed_t dx;
		fixed_t dy;
		fixed_t dist;

		dx = abs(thing->x - bombspot->x);
		dy = abs(thing->y - bombspot->y);

		dist = dx>dy ? dx : dy;
		dist = (dist - thing->radius) >> FRACBITS;

		if (dist >= bombdamage)
			return true;  // out of range

		if (dist < 0)
			dist = 0;

		if ( P_CheckSight (thing, bombspot, true) )
		{
			// must be in direct path
			P_DamageMobj (thing, bombspot, bombsource, bombdamage - dist, bombmod);
		}
	}

	return true;
}


//
// P_RadiusAttack
// Source is the creature that caused the explosion at spot.
//
void P_RadiusAttack (mobj_t *spot, mobj_t *source, int damage, int mod)
{
	int 		x;
	int 		y;
	
	int 		xl;
	int 		xh;
	int 		yl;
	int 		yh;
	
	fixed_t 	dist;
		
	dist = (damage+MAXRADIUS)<<FRACBITS;
	yh = (spot->y + dist - bmaporgy)>>MAPBLOCKSHIFT;
	yl = (spot->y - dist - bmaporgy)>>MAPBLOCKSHIFT;
	xh = (spot->x + dist - bmaporgx)>>MAPBLOCKSHIFT;
	xl = (spot->x - dist - bmaporgx)>>MAPBLOCKSHIFT;
	bombspot = spot;
	bombsource = source;
	bombdamage = damage;
	bombdamagefloat = (float)damage;
	bombmod = mod;
	VectorPosition (spot, bombvec);
		
	for (y=yl ; y<=yh ; y++)
		for (x=xl ; x<=xh ; x++)
			P_BlockThingsIterator (x, y, PIT_RadiusAttack );
}



//
// SECTOR HEIGHT CHANGING
// After modifying a sectors floor or ceiling height,
// call this routine to adjust the positions
// of all things that touch the sector.
//
// If anything doesn't fit anymore, true will be returned.
//
// [RH] If crushchange is non-negative, they will take the
//		specified amount of damage as they are being crushed.
//		If crushchange is negative, you should set the sector
//		height back the way it was and call P_ChangeSector
//		again to undo the changes.
//		Note that this is very different from the original
//		true/false usage of crushchange! If you want regular
//		DOOM crushing behavior set crushchange to 10 or -1
//		if no crushing is desired.
//
int		crushchange;
BOOL 	nofit;


//
// PIT_ChangeSector
//
BOOL PIT_ChangeSector (mobj_t *thing)
{
	mobj_t *mo;
	int t;
		
	if (P_ThingHeightClip (thing))
	{
		// keep checking
		return true;
	}
	

	// crunch bodies to giblets
	if (thing->health <= 0)
	{
		P_SetMobjState (thing, S_GIBS);

		thing->flags &= ~MF_SOLID;
		thing->height = 0;
		thing->radius = 0;

		// keep checking
		return true;			
	}

	// crunch dropped items
	if (thing->flags & MF_DROPPED)
	{
		P_RemoveMobj (thing);
		
		// keep checking
		return true;			
	}

	if (! (thing->flags & MF_SHOOTABLE) )
	{
		// assume it is bloody gibs or something
		return true;					
	}
	
	nofit = true;

	if ((crushchange >= 0) && !(level.time&3) )
	{
		P_DamageMobj (thing, NULL, NULL, crushchange, MOD_CRUSH);

		// spray blood in a random direction
		mo = P_SpawnMobj (thing->x,
						  thing->y,
						  thing->z + thing->height/2, MT_BLOOD, 0);
		
		t = P_Random (pr_changesector);
		mo->momx = (t - P_Random (pr_changesector)) << 12;
		t = P_Random (pr_changesector);
		mo->momy = (t - P_Random (pr_changesector)) << 12;
	}

	// keep checking (crush other things)		
	return true;		
}



//
// P_ChangeSector
//
BOOL P_ChangeSector (sector_t *sector, int crunch)
{
	int x;
	int y;
		
	nofit = false;
	crushchange = crunch;

	// ARRGGHHH!!!!
	// This is horrendously slow!!!
	// killough 3/14/98

	// re-check heights for all things near the moving sector
	for (x=sector->blockbox[BOXLEFT] ; x<= sector->blockbox[BOXRIGHT] ; x++)
		for (y=sector->blockbox[BOXBOTTOM];y<= sector->blockbox[BOXTOP] ; y++)
			P_BlockThingsIterator (x, y, PIT_ChangeSector);
		
	return nofit;
}

//
// P_CheckSector
// jff 3/19/98 added to just check monsters on the periphery
// of a moving sector instead of all in bounding box of the
// sector. Both more accurate and faster.
//

BOOL P_CheckSector (sector_t *sector, int crunch)
{
	msecnode_t *n;

	if (olddemo) // use the old routine for old demos though
		return P_ChangeSector (sector, crunch);

	nofit = false;
	crushchange = crunch;

	// killough 4/4/98: scan list front-to-back until empty or exhausted,
	// restarting from beginning after each thing is processed. Avoids
	// crashes, and is sure to examine all things in the sector, and only
	// the things which are in the sector, until a steady-state is reached.
	// Things can arbitrarily be inserted and removed and it won't mess up.
	//
	// killough 4/7/98: simplified to avoid using complicated counter

	// Mark all things invalid

	for (n=sector->touching_thinglist; n; n=n->m_snext)
		n->visited = false;

	do
		for (n=sector->touching_thinglist; n; n=n->m_snext)	// go through list
			if (!n->visited)								// unprocessed thing found
			{
				n->visited	= true; 						// mark thing as processed
				if (!(n->m_thing->flags & MF_NOBLOCKMAP))	//jff 4/7/98 don't do these
					PIT_ChangeSector(n->m_thing); 			// process it
				break;										// exit and start over
			}
	while (n);	// repeat from scratch until all things left are marked valid

	return nofit;
}


// phares 3/21/98
//
// Maintain a freelist of msecnode_t's to reduce memory allocs and frees.

msecnode_t *headsecnode = NULL;

// P_GetSecnode() retrieves a node from the freelist. The calling routine
// should make sure it sets all fields properly.

msecnode_t *P_GetSecnode()
{
	msecnode_t *node;

	if (headsecnode)
	{
		node = headsecnode;
		headsecnode = headsecnode->m_snext;
	}
	else
		node = Z_Malloc (sizeof(*node), PU_LEVEL, NULL);
	return(node);
}

// P_PutSecnode() returns a node to the freelist.

void P_PutSecnode (msecnode_t* node)
{
	node->m_snext = headsecnode;
	headsecnode = node;
}

// phares 3/16/98
//
// P_AddSecnode() searches the current list to see if this sector is
// already there. If not, it adds a sector node at the head of the list of
// sectors this object appears in. This is called when creating a list of
// nodes that will get linked in later. Returns a pointer to the new node.

msecnode_t* P_AddSecnode(sector_t* s, mobj_t* thing, msecnode_t* nextnode)
{
	msecnode_t* node;

	node = nextnode;
	while (node)
	{
		if (node->m_sector == s)	// Already have a node for this sector?
		{
			node->m_thing = thing;	// Yes. Setting m_thing says 'keep it'.
			return(nextnode);
		}
		node = node->m_tnext;
	}

	// Couldn't find an existing node for this sector. Add one at the head
	// of the list.

	node = P_GetSecnode();

	// killough 4/4/98, 4/7/98: mark new nodes unvisited.
	node->visited = 0;

	node->m_sector = s; 			// sector
	node->m_thing  = thing; 		// mobj
	node->m_tprev  = NULL;			// prev node on Thing thread
	node->m_tnext  = nextnode;		// next node on Thing thread
	if (nextnode)
		nextnode->m_tprev = node;	// set back link on Thing

	// Add new node at head of sector thread starting at s->touching_thinglist

	node->m_sprev  = NULL;			// prev node on sector thread
	node->m_snext  = s->touching_thinglist; // next node on sector thread
	if (s->touching_thinglist)
		node->m_snext->m_sprev = node;
	s->touching_thinglist = node;
	return(node);
}


// P_DelSecnode() deletes a sector node from the list of
// sectors this object appears in. Returns a pointer to the next node
// on the linked list, or NULL.

msecnode_t* P_DelSecnode(msecnode_t* node)
{
	msecnode_t* tp;  // prev node on thing thread
	msecnode_t* tn;  // next node on thing thread
	msecnode_t* sp;  // prev node on sector thread
	msecnode_t* sn;  // next node on sector thread

	if (node)
	{

		// Unlink from the Thing thread. The Thing thread begins at
		// sector_list and not from mobj_t->touching_sectorlist.

		tp = node->m_tprev;
		tn = node->m_tnext;
		if (tp)
			tp->m_tnext = tn;
		if (tn)
			tn->m_tprev = tp;

		// Unlink from the sector thread. This thread begins at
		// sector_t->touching_thinglist.

		sp = node->m_sprev;
		sn = node->m_snext;
		if (sp)
			sp->m_snext = sn;
		else
			node->m_sector->touching_thinglist = sn;
		if (sn)
			sn->m_sprev = sp;

		// Return this node to the freelist

		P_PutSecnode(node);
		return(tn);
	}
	return(NULL);
} 														// phares 3/13/98

// Delete an entire sector list

void P_DelSeclist(msecnode_t* node)
{
	while (node)
		node = P_DelSecnode(node);
}


// phares 3/14/98
//
// PIT_GetSectors
// Locates all the sectors the object is in by looking at the lines that
// cross through it. You have already decided that the object is allowed
// at this location, so don't bother with checking impassable or
// blocking lines.

BOOL PIT_GetSectors(line_t* ld)
{
	if (tmbbox[BOXRIGHT]	  <= ld->bbox[BOXLEFT]	 ||
			tmbbox[BOXLEFT]   >= ld->bbox[BOXRIGHT]  ||
			tmbbox[BOXTOP]	  <= ld->bbox[BOXBOTTOM] ||
			tmbbox[BOXBOTTOM] >= ld->bbox[BOXTOP])
		return true;

	if (P_BoxOnLineSide(tmbbox, ld) != -1)
		return true;

	// This line crosses through the object.

	// Collect the sector(s) from the line and add to the
	// sector_list you're examining. If the Thing ends up being
	// allowed to move to this position, then the sector_list
	// will be attached to the Thing's mobj_t at touching_sectorlist.

	sector_list = P_AddSecnode(ld->frontsector,tmthing,sector_list);

	// Don't assume all lines are 2-sided, since some Things
	// like MT_TFOG are allowed regardless of whether their radius takes
	// them beyond an impassable linedef.

	// killough 3/27/98, 4/4/98:
	// Use sidedefs instead of 2s flag to determine two-sidedness.

	if (ld->backsector)
		sector_list = P_AddSecnode(ld->backsector, tmthing, sector_list);

	return true;
}


// phares 3/14/98
//
// P_CreateSecNodeList alters/creates the sector_list that shows what sectors
// the object resides in.

void P_CreateSecNodeList(mobj_t* thing,fixed_t x,fixed_t y)
{
	int xl;
	int xh;
	int yl;
	int yh;
	int bx;
	int by;
	msecnode_t* node;

	// First, clear out the existing m_thing fields. As each node is
	// added or verified as needed, m_thing will be set properly. When
	// finished, delete all nodes where m_thing is still NULL. These
	// represent the sectors the Thing has vacated.

	node = sector_list;
	while (node)
	{
		node->m_thing = NULL;
		node = node->m_tnext;
	}

	tmthing = thing;
	tmflags = thing->flags;

	tmx = x;
	tmy = y;

	tmbbox[BOXTOP]	= y + tmthing->radius;
	tmbbox[BOXBOTTOM] = y - tmthing->radius;
	tmbbox[BOXRIGHT]	= x + tmthing->radius;
	tmbbox[BOXLEFT] 	= x - tmthing->radius;

	validcount++; // used to make sure we only process a line once

	xl = (tmbbox[BOXLEFT] - bmaporgx)>>MAPBLOCKSHIFT;
	xh = (tmbbox[BOXRIGHT] - bmaporgx)>>MAPBLOCKSHIFT;
	yl = (tmbbox[BOXBOTTOM] - bmaporgy)>>MAPBLOCKSHIFT;
	yh = (tmbbox[BOXTOP] - bmaporgy)>>MAPBLOCKSHIFT;

	for (bx=xl ; bx<=xh ; bx++)
		for (by=yl ; by<=yh ; by++)
			P_BlockLinesIterator(bx,by,PIT_GetSectors);

	// Add the sector of the (x,y) point to sector_list.

	sector_list = P_AddSecnode(thing->subsector->sector,thing,sector_list);

	// Now delete any nodes that won't be used. These are the ones where
	// m_thing is still NULL.

	node = sector_list;
	while (node)
	{
		if (node->m_thing == NULL)
		{
			if (node == sector_list)
				sector_list = node->m_tnext;
			node = P_DelSecnode(node);
		}
		else
			node = node->m_tnext;
	}
}
