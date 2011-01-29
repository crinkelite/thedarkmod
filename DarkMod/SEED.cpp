// vim:ts=4:sw=4:cindent
/***************************************************************************
 *
 * PROJECT: The Dark Mod
 * $Revision$
 * $Date$
 * $Author$
 *
 ***************************************************************************/

// Copyright (C) 2010-2011 Tels (Donated to The Dark Mod Team)

/*
  System for Environmental Entity Distribution (SEED, formerly known as LODE)

  Manage other entities based on LOD (e.g. distance), as well as create entities
  based on rules in semi-random places/rotations/sizes and colors.

Important things to do:

TODO: #2571: Restore() crashes if you combine func_statics (works fine with combine = 0)

Nice-to-have:

TODO: add console command to save all SEED entities as prefab?
TODO: take over LOD changes from entity
TODO: add a "pseudoclass" bit so into the entity flags field, so we can use much
	  smaller structs for pseudo classes (we might have thousands
	  of pseudoclass structs due to each having a different hmodel)
TODO: add "watch_models" (or "combine_models"?) so the mapper can place models and
	  then use the modelgenerator to combine them into one big rendermodel. The current
	  way of targeting and using "watch_brethren" does get all "func_static" as it is
	  classname based, not model name based.

Optimizations:

TODO: Make the max-combine-distance setting depending on the LOD stages, if the model
	  has none + hide, or none and no hide, we can use a bigger distance because the
	  rendermodel will be less often (or never) be rebuild. Also benchmark wether
	  a bigger distance is slower/faster even with rendermodel rebuilds.
TODO: Make it so we can also combine entities from different classes, e.g. not just
	  take their class and skin, instead build a set of surfaces for each entities
	  *after* the skin is applied, then use this set for matching. F.i. cattails2
	  and cattails4 can be combined as they have the same surface, but cattails2-yellow
	  cannot, as they don't share a surface. This will lead to better combining and
	  thus reduce the drawcalls. Problem: different clipmodels and LOD stages might
	  prevent this from working - but it could work for non-solids and things without
	  any LOD stages at all (like the cattails).
TODO: add a "entity" field (int) to the offsets list, so we avoid having to
	  construct a sortOffsets list first, then truncated and rebuild the offsets
	  list from that. (But benchmark if that isn't actually faster as the sortedOffsets
	  list contans only one int and a ptr)
TODO: Sort all the generated entities into multiple lists, keyed on a hash-key that
	  contains all the relevant info for combining entities, so only entities that
	  could be combined have the same hash key. F.i. "skin-name,model-name,class-name,etc".
	  Then only look at entities from one list when combining, this will reduce the
	  O(N*N) to something like O( (N/X)*(N/X) ) where X is the set of combinable entities.
TODO: Use a point (at least for nonsolids or vegetation?) instead of a box when determining
	  the underlying material/placement - this could be much faster.
*/

#include "../idlib/precompiled.h"
#pragma hdrstop

// define to output model generation debug info
//#define M_DEBUG

static bool init_version = FileVersionList("$Id$", init_version);

#include "../game/game_local.h"
#include "../idlib/containers/list.h"
#include "SEED.h"

// maximum number of tries to place an entity
#define MAX_TRIES 8

// the name of the class where to look for shared models
#define FUNC_STATIC "func_static"
// the name of the dummy func static with a visual model
// Used because I could not get it to work with spawning an
// empty func_static, then adding the model (modelDefHandle is protected)
#define FUNC_DUMMY "atdm:seed_dummy_static"

// Avoid that we run out of entities:

// TODO: if the number of entities is higher then this,
// then we favour to spawn big(ger) entities over smaller ones
#define SPAWN_SMALL_LIMIT (MAX_GENTITIES - 500)

// if the number of entities is higher than this, we no longer spawn entities
#define SPAWN_LIMIT (MAX_GENTITIES - 100)

const idEventDef EV_CullAll( "cullAll", "" );

/*
===============================================================================
*/

CLASS_DECLARATION( idStaticEntity, Seed )
	EVENT( EV_Activate,				Seed::Event_Activate )
	EVENT( EV_Enable,				Seed::Event_Enable )
	EVENT( EV_Disable,				Seed::Event_Disable )
	EVENT( EV_CullAll,				Seed::Event_CullAll )
END_CLASS

/*
===============
Seed::Seed
===============
*/
Seed::Seed( void ) {

	active = false;

	m_iSeed = 3;
	m_iSeed_2 = 7;
	m_iOrgSeed = 7;
	m_fLODBias = 0;

	m_iDebug = 0;
	m_bDebugColors = false;

	m_bWaitForTrigger = false;

	m_bPrepared = false;
	m_Entities.Clear();
	m_Classes.Clear();
	m_Inhibitors.Clear();

	m_iNumStaticMulties = 0;
	m_bRestoreLOD = false;

	m_bCombine = true;

	// always put the empty skin into the list so it has index 0
	m_Skins.Clear();
	m_Skins.Append ( idStr("") );

	m_iNumEntities = 0;
	m_iNumExisting = 0;
	m_iNumVisible = 0;

	m_iNumPVSAreas = 0;
	m_iThinkCounter = 0;

	m_DistCheckTimeStamp = 0;
	m_DistCheckInterval = 0.5f;
	m_bDistCheckXYOnly = false;
}

/*
===============
Seed::~Seed
===============
*/
Seed::~Seed(void) {

	//gameLocal.Warning ("SEED %s: Shutdown.\n", GetName() );
	ClearClasses();
}
/*
===============
Seed::Save
===============
*/
void Seed::Save( idSaveGame *savefile ) const {

	savefile->WriteBool( active );
	savefile->WriteBool( m_bWaitForTrigger );

	savefile->WriteInt( m_iDebug );
	savefile->WriteBool( m_bDebugColors );

	savefile->WriteBool( m_bCombine );

	savefile->WriteInt( m_iSeed );
	savefile->WriteInt( m_iSeed_2 );
	savefile->WriteInt( m_iOrgSeed );
	savefile->WriteInt( m_iNumEntities );
	savefile->WriteInt( m_iNumExisting );
	savefile->WriteInt( m_iNumVisible );
	savefile->WriteInt( m_iThinkCounter );
	savefile->WriteFloat( m_fLODBias );

    savefile->WriteInt( m_DistCheckTimeStamp );
	savefile->WriteInt( m_DistCheckInterval );
	savefile->WriteBool( m_bDistCheckXYOnly );

	savefile->WriteVec3( m_origin );

	savefile->WriteInt( m_iNumStaticMulties );

	savefile->WriteInt( m_Entities.Num() );
	for( int i = 0; i < m_Entities.Num(); i++ )
	{
		savefile->WriteInt( m_Entities[i].skinIdx );
		savefile->WriteVec3( m_Entities[i].origin );
		savefile->WriteAngles( m_Entities[i].angles );
		// a dword is "unsigned int"
		savefile->WriteInt( m_Entities[i].color );
		savefile->WriteInt( m_Entities[i].flags );
		savefile->WriteInt( m_Entities[i].entity );
		savefile->WriteInt( m_Entities[i].classIdx );
	}

	savefile->WriteInt( m_Classes.Num() );
	for( int i = 0; i < m_Classes.Num(); i++ )
	{
		savefile->WriteString( m_Classes[i].classname );
		savefile->WriteString( m_Classes[i].modelname );
		savefile->WriteBool( m_Classes[i].pseudo );
		savefile->WriteBool( m_Classes[i].watch );

		savefile->WriteInt( m_Classes[i].maxEntities );
		savefile->WriteInt( m_Classes[i].numEntities );
		savefile->WriteInt( m_Classes[i].score );

		if (m_Classes[i].clip)
		{
			savefile->WriteBool( true );
			savefile->WriteClipModel( m_Classes[i].clip );
		}
		else
		{
			savefile->WriteBool( false );
		}
		savefile->WriteFloat( m_Classes[i].cullDist );
		savefile->WriteFloat( m_Classes[i].spawnDist );
		savefile->WriteFloat( m_Classes[i].spacing );
		savefile->WriteFloat( m_Classes[i].bunching );
		savefile->WriteFloat( m_Classes[i].sink_min );
		savefile->WriteFloat( m_Classes[i].sink_max );
		savefile->WriteVec3( m_Classes[i].scale_min );
		savefile->WriteVec3( m_Classes[i].scale_max );
		savefile->WriteVec3( m_Classes[i].origin );
		savefile->WriteVec3( m_Classes[i].offset );
		savefile->WriteInt( m_Classes[i].nocollide );
		savefile->WriteBool( m_Classes[i].nocombine );
		savefile->WriteBool( m_Classes[i].solid );
		savefile->WriteInt( m_Classes[i].falloff );
		savefile->WriteBool( m_Classes[i].floor );
		savefile->WriteBool( m_Classes[i].stack );
		savefile->WriteBool( m_Classes[i].noinhibit );
		savefile->WriteVec3( m_Classes[i].size );
		savefile->WriteFloat( m_Classes[i].avgSize );
		savefile->WriteVec3( m_Classes[i].color_min );
		savefile->WriteVec3( m_Classes[i].color_max );
		savefile->WriteBool( m_Classes[i].z_invert );
		savefile->WriteFloat( m_Classes[i].z_min );
		savefile->WriteFloat( m_Classes[i].z_max );
		savefile->WriteFloat( m_Classes[i].z_fadein );
		savefile->WriteFloat( m_Classes[i].z_fadeout );

		savefile->WriteFloat( m_Classes[i].defaultProb );

		savefile->WriteInt( m_Classes[i].skins.Num() );
		for( int j = 0; j < m_Classes[i].skins.Num(); j++ )
		{
			savefile->WriteInt( m_Classes[i].skins[j] );
		}

		savefile->WriteInt( m_Classes[i].materials.Num() );
		for( int j = 0; j < m_Classes[i].materials.Num(); j++ )
		{
			savefile->WriteString( m_Classes[i].materials[j].name );
			savefile->WriteFloat( m_Classes[i].materials[j].probability );
		}

		// only save these if they are used
		if ( m_Classes[i].falloff == 5)
		{
			savefile->WriteFloat( m_Classes[i].func_x );
			savefile->WriteFloat( m_Classes[i].func_y );
			savefile->WriteFloat( m_Classes[i].func_s );
			savefile->WriteFloat( m_Classes[i].func_a );
			savefile->WriteInt( m_Classes[i].func_Xt );
			savefile->WriteInt( m_Classes[i].func_Yt );
			savefile->WriteInt( m_Classes[i].func_f );
			savefile->WriteFloat( m_Classes[i].func_min );
			savefile->WriteFloat( m_Classes[i].func_max );
		}
		if ( m_Classes[i].falloff >= 2 && m_Classes[i].falloff <= 3)
		{
			savefile->WriteFloat( m_Classes[i].func_a );
		}
		// image based distribution
		savefile->WriteUnsignedInt( m_Classes[i].imgmap );
		if (m_Classes[i].imgmap != 0)
		{
			savefile->WriteBool( m_Classes[i].map_invert );
			savefile->WriteFloat( m_Classes[i].map_scale_x );
			savefile->WriteFloat( m_Classes[i].map_scale_y );
			savefile->WriteFloat( m_Classes[i].map_ofs_x );
			savefile->WriteFloat( m_Classes[i].map_ofs_y );
		}

		// only write the rendermodel if it is used
		if ( NULL != m_Classes[i].hModel)
		{
			savefile->WriteBool( true );
			savefile->WriteModel( m_Classes[i].hModel );
		}
		else
		{
			savefile->WriteBool( false );
		}
		// only write the clipmodel if it is used
		if ( NULL != m_Classes[i].physicsObj)
		{
			savefile->WriteBool( true );
			m_Classes[i].physicsObj->Save( savefile );
		}
		else
		{
			savefile->WriteBool( false );
		}
	}
	savefile->WriteInt( m_Inhibitors.Num() );
	for( int i = 0; i < m_Inhibitors.Num(); i++ )
	{
		savefile->WriteVec3( m_Inhibitors[i].origin );
		savefile->WriteVec3( m_Inhibitors[i].size );
		savefile->WriteBox( m_Inhibitors[i].box );
		savefile->WriteBool( m_Inhibitors[i].inhibit_only );
		savefile->WriteInt( m_Inhibitors[i].falloff );
		savefile->WriteFloat( m_Inhibitors[i].factor );
		int n = m_Inhibitors[i].classnames.Num();
		savefile->WriteInt( n );
		for( int j = 0; j < n; j++ )
		{
			savefile->WriteString( m_Inhibitors[i].classnames[j] );
		}
	}
	savefile->WriteInt( m_Skins.Num() );
	for( int i = 0; i < m_Skins.Num(); i++ )
	{
		savefile->WriteString( m_Skins[i] );
	}

	savefile->WriteInt( m_iNumPVSAreas );
	for( int i = 0; i < m_iNumPVSAreas; i++ )
	{
		savefile->WriteInt( m_iPVSAreas[i] );
	}
}

/*
===============
Seed::ClearClasses

Free memory from render models and Images
===============
*/
void Seed::ClearClasses( void )
{
	int n = m_Classes.Num();
	for(int i = 0; i < n; i++ )
	{
		if (NULL != m_Classes[i].hModel)
		{
			if (m_Classes[i].pseudo && m_Classes[i].hModel)
			{
				renderModelManager->FreeModel( m_Classes[i].hModel );
			}
			m_Classes[i].hModel = NULL;
		}
		if (NULL != m_Classes[i].physicsObj)
		{
			// avoid double free:
			//delete m_Classes[i].physicsObj;
			m_Classes[i].physicsObj = NULL;
		}
		if (m_Classes[i].imgmap != 0)
		{
			gameLocal.m_ImageMapManager->UnregisterMap( m_Classes[i].imgmap );
		}
	}
	m_Classes.Clear();
	m_iNumStaticMulties = 0;
}

/*
===============
Seed::Restore
===============
*/
void Seed::Restore( idRestoreGame *savefile ) {
	int num;
	int numClasses;
	bool bHaveModel;

	savefile->ReadBool( active );
	savefile->ReadBool( m_bWaitForTrigger );

	savefile->ReadInt( m_iDebug );
	savefile->ReadBool( m_bDebugColors );

	savefile->ReadBool( m_bCombine );

	savefile->ReadInt( m_iSeed );
	savefile->ReadInt( m_iSeed_2 );
	savefile->ReadInt( m_iOrgSeed );
	savefile->ReadInt( m_iNumEntities );
	savefile->ReadInt( m_iNumExisting );
	savefile->ReadInt( m_iNumVisible );
	savefile->ReadInt( m_iThinkCounter );
	savefile->ReadFloat( m_fLODBias );
	
    savefile->ReadInt( m_DistCheckTimeStamp );
	savefile->ReadInt( m_DistCheckInterval );
	savefile->ReadBool( m_bDistCheckXYOnly );

	savefile->ReadVec3( m_origin );

	savefile->ReadInt( m_iNumStaticMulties );
	// do the SetLODData() once in Think()
	m_bRestoreLOD = true;

    savefile->ReadInt( num );
	m_Entities.Clear();
	m_Entities.SetNum( num );
	int clr;
	for( int i = 0; i < num; i++ )
	{
		savefile->ReadInt( m_Entities[i].skinIdx );
		savefile->ReadVec3( m_Entities[i].origin );
		savefile->ReadAngles( m_Entities[i].angles );
		// a dword is "unsigned int"
		savefile->ReadInt( clr );
		m_Entities[i].color = (dword)clr;
		savefile->ReadInt( m_Entities[i].flags );
		savefile->ReadInt( m_Entities[i].entity );
		savefile->ReadInt( m_Entities[i].classIdx );
	}

    savefile->ReadInt( numClasses );
	// clear m_Classes and free any models in it, too
	ClearClasses();
	m_Classes.SetNum( numClasses );
	for( int i = 0; i < numClasses; i++ )
	{
		savefile->ReadString( m_Classes[i].classname );
		savefile->ReadString( m_Classes[i].modelname );
		savefile->ReadBool( m_Classes[i].pseudo );
		savefile->ReadBool( m_Classes[i].watch );

		savefile->ReadInt( m_Classes[i].maxEntities );
		savefile->ReadInt( m_Classes[i].numEntities );
		savefile->ReadInt( m_Classes[i].score );

		savefile->ReadBool( bHaveModel );
		m_Classes[i].clip = NULL;
		// only read the clip model if it is actually used
		if ( bHaveModel )
		{
			savefile->ReadClipModel( m_Classes[i].clip );
		}

		savefile->ReadFloat( m_Classes[i].cullDist );
		savefile->ReadFloat( m_Classes[i].spawnDist );
		savefile->ReadFloat( m_Classes[i].spacing );
		savefile->ReadFloat( m_Classes[i].bunching );
		savefile->ReadFloat( m_Classes[i].sink_min );
		savefile->ReadFloat( m_Classes[i].sink_max );
		savefile->ReadVec3( m_Classes[i].scale_min );
		savefile->ReadVec3( m_Classes[i].scale_max );
		savefile->ReadVec3( m_Classes[i].origin );
		savefile->ReadVec3( m_Classes[i].offset );
		savefile->ReadInt( m_Classes[i].nocollide );
		savefile->ReadBool( m_Classes[i].nocombine );
		savefile->ReadBool( m_Classes[i].solid );
		savefile->ReadInt( m_Classes[i].falloff );
		savefile->ReadBool( m_Classes[i].floor );
		savefile->ReadBool( m_Classes[i].stack );
		savefile->ReadBool( m_Classes[i].noinhibit );
		savefile->ReadVec3( m_Classes[i].size );
		savefile->ReadFloat( m_Classes[i].avgSize );
		savefile->ReadVec3( m_Classes[i].color_min );
		savefile->ReadVec3( m_Classes[i].color_max );
		savefile->ReadBool( m_Classes[i].z_invert );
		savefile->ReadFloat( m_Classes[i].z_min );
		savefile->ReadFloat( m_Classes[i].z_max );
		savefile->ReadFloat( m_Classes[i].z_fadein );
		savefile->ReadFloat( m_Classes[i].z_fadeout );

		savefile->ReadFloat( m_Classes[i].defaultProb );

		savefile->ReadInt( num );
		m_Classes[i].skins.Clear();
		m_Classes[i].skins.SetNum( num );
		for( int j = 0; j < num; j++ )
		{
			savefile->ReadInt( m_Classes[i].skins[j] );
		}

		savefile->ReadInt( num );
		m_Classes[i].materials.Clear();
		m_Classes[i].materials.SetNum( num );
		for( int j = 0; j < num; j++ )
		{
			savefile->ReadString( m_Classes[i].materials[j].name );
			savefile->ReadFloat( m_Classes[i].materials[j].probability );
		}

		// only restore these if they are used
		if ( m_Classes[i].falloff == 5)
		{
			savefile->ReadFloat( m_Classes[i].func_x );
			savefile->ReadFloat( m_Classes[i].func_y );
			savefile->ReadFloat( m_Classes[i].func_s );
			savefile->ReadFloat( m_Classes[i].func_a );
			savefile->ReadInt( m_Classes[i].func_Xt );
			savefile->ReadInt( m_Classes[i].func_Yt );
			savefile->ReadInt( m_Classes[i].func_f );
			savefile->ReadFloat( m_Classes[i].func_min );
			savefile->ReadFloat( m_Classes[i].func_max );
		}
		if ( m_Classes[i].falloff >= 2 && m_Classes[i].falloff <= 3)
		{
			savefile->ReadFloat( m_Classes[i].func_a );
		}
		m_Classes[i].map_invert = false;
		m_Classes[i].map_scale_x = 1.0f;
		m_Classes[i].map_scale_y = 1.0f;
		m_Classes[i].map_ofs_x = 0.0f;
		m_Classes[i].map_ofs_y = 0.0f;

		savefile->ReadUnsignedInt( m_Classes[i].imgmap );
	    if (m_Classes[i].imgmap != 0)
		{
			savefile->ReadBool( m_Classes[i].map_invert );
			savefile->ReadFloat( m_Classes[i].map_scale_x );
			savefile->ReadFloat( m_Classes[i].map_scale_y );
			savefile->ReadFloat( m_Classes[i].map_ofs_x );
			savefile->ReadFloat( m_Classes[i].map_ofs_y );
		}

		savefile->ReadBool( bHaveModel );
		m_Classes[i].hModel = NULL;
		// only read the model if it is actually used
		if ( bHaveModel )
		{
			savefile->ReadModel( m_Classes[i].hModel );
		}

		savefile->ReadBool( bHaveModel );
		m_Classes[i].physicsObj = NULL;

		// only read the model if it is actually used
		if ( bHaveModel )
		{
			m_Classes[i].physicsObj = new idPhysics_StaticMulti;
			m_Classes[i].physicsObj->Restore( savefile );
		}
	}

	savefile->ReadInt( num );
	m_Inhibitors.Clear();
	m_Inhibitors.SetNum( num );
	for( int i = 0; i < num; i++ )
	{
		savefile->ReadVec3( m_Inhibitors[i].origin );
		savefile->ReadVec3( m_Inhibitors[i].size );
		savefile->ReadBox( m_Inhibitors[i].box );
		savefile->ReadBool( m_Inhibitors[i].inhibit_only );
		savefile->ReadInt( m_Inhibitors[i].falloff );
		savefile->ReadFloat( m_Inhibitors[i].factor );
		int n;
		savefile->ReadInt( n );
		m_Inhibitors[i].classnames.Clear();
		m_Inhibitors[i].classnames.SetNum(n);
		for( int j = 0; j < n; j++ )
		{
			savefile->ReadString( m_Inhibitors[i].classnames[j] );
		}
	}

	savefile->ReadInt( num );
	m_Skins.Clear();
	m_Skins.SetNum( num );
	for( int i = 0; i < num; i++ )
	{
		savefile->ReadString( m_Skins[i] );
	}

	savefile->ReadInt( m_iNumPVSAreas );
	for( int i = 0; i < m_iNumPVSAreas; i++ )
	{
		savefile->ReadInt( m_iPVSAreas[i] );
	}
}

/*
===============
Seed::RandomSeed

Implement our own, independent random generator with our own seed, so we are
independent from the seed in gameLocal and the one used in RandomFloat. This
one is used to calculate the seeds per-class, values choosen per:
http://en.wikipedia.org/wiki/Linear_congruential_generator
===============
*/
ID_INLINE int Seed::RandomSeed( void ) {
	m_iSeed_2 = 1103515245L * m_iSeed_2 + 12345L;
	return m_iSeed_2 & 0x7FFFFFF;
}

/*
===============
Seed::RandomFloat

Implement our own random generator with our own seed, so we are independent
from the seed in gameLocal, also needs to be independent from Seed::RandomSeed:
http://en.wikipedia.org/wiki/Linear_congruential_generator
===============
*/
ID_INLINE float Seed::RandomFloat( void ) {
	unsigned long i;
	m_iSeed = 1664525L * m_iSeed + 1013904223L;
	i = Seed::IEEE_ONE | ( m_iSeed & Seed::IEEE_MASK );
	return ( ( *(float *)&i ) - 1.0f );
}

/*
===============
Seed::Spawn
===============
*/
void Seed::Spawn( void ) {

	// DEBUG
//	gameLocal.Printf( "SEED %s: Sizes: seed_entity_t %i, seed_class_t %i, lod_data_t %i, idEntity %i, idStaticEntity %i, CImage %i.\n", 
//			GetName(), sizeof(seed_entity_t), sizeof(seed_class_t), sizeof(lod_data_t), sizeof(idEntity), sizeof(idStaticEntity), sizeof(CImage) );

	// if we subtract the render entity origin from the physics origin (this is where the mapper places
	// the origin inside DR), we magically arrive at the true origin of the visible brush placed in DR.
	// Don't ask my why and don't mention I found this out after several days. Signed, a mellow Tels.
	m_origin = GetPhysics()->GetOrigin() + renderEntity.bounds.GetCenter();

//	idClipModel *clip = GetPhysics()->GetClipModel();
//	idVec3 o = clip->GetOrigin();
//	idVec3 s = clip->GetBounds().GetSize();
//	idAngles a = clip->GetAxis().ToAngles();
//	gameLocal.Printf( "SEED %s: Clipmodel origin %0.2f %0.2f %0.2f size %0.2f %0.2f %0.2f axis %s.\n", GetName(), o.x, o.y, o.z, s.x, s.y, s.z, a.ToString() );

//	idTraceModel *trace = GetPhysics()->GetClipModel()->GetTraceModel();
//	idVec3 o = trace->GetOrigin();
//	idVec3 s = trace->GetBounds().GetSize();
//	idAngles a = trace->GetAxis().ToAngles();

//	gameLocal.Printf( "SEED %s: Tracemodel origin %0.2f %0.2f %0.2f size %0.2f %0.2f %0.2f axis %s.\n", GetName(), o.x, o.y, o.z, s.x, s.y, s.z, a.ToString() );

	idVec3 size = renderEntity.bounds.GetSize();
	idAngles angles = renderEntity.axis.ToAngles();

	// cache in which PVS(s) we are, so we can later check if we are in Player PVS
	// calculate our bounds

	idBounds b = idBounds( - size / 2, size / 2 );	// a size/2 bunds centered around 0, will below move to m_origin
	idBounds modelAbsBounds;
    modelAbsBounds.FromTransformedBounds( b, m_origin, renderEntity.axis );
	m_iNumPVSAreas = gameLocal.pvs.GetPVSAreas( modelAbsBounds, m_iPVSAreas, sizeof( m_iPVSAreas ) / sizeof( m_iPVSAreas[0] ) );

	gameLocal.Printf( "SEED %s: Seed %i Size %0.2f %0.2f %0.2f Axis %s, PVS count %i.\n", GetName(), m_iSeed, size.x, size.y, size.z, angles.ToString(), m_iNumPVSAreas );

/*
	// how to get the current model (rough outline)

   get number of surfaces:
	// NumBaseSurfaces will not count any overlays added to dynamic models
    virtual int                                     NumBaseSurfaces() const = 0;

   get all surfaces:
    // get a pointer to a surface
    virtual const modelSurface_t *Surface( int surfaceNum ) const = 0;

   for each surface, get all vertices

   then from these vertices, create a vertic-list and from this a trcemodel:

	idTraceModel trace = idTraceModel();
	trace.SetupPolygon( vertices, int );
*/

	// the Seed itself is sneaky and hides itself
	Hide();

	// And is nonsolid, too!
	GetPhysics()->SetContents( 0 );

	m_fLODBias = cv_lod_bias.GetFloat();

	active = true;

	m_iDebug = spawnArgs.GetInt( "debug", "0" );
	m_bDebugColors = spawnArgs.GetBool( "debug_colors", "0" );

	// default is to combine
	m_bCombine = spawnArgs.GetBool("combine", "1");

	m_bWaitForTrigger = spawnArgs.GetBool("wait_for_trigger", "0");

	m_DistCheckInterval = (int) (1000.0f * spawnArgs.GetFloat( "dist_check_period", "0.05" ));

	float cullRange = spawnArgs.GetFloat( "cull_range", "150" );
	gameLocal.Printf ("SEED %s: cull range = %0.2f.\n", GetName(), cullRange );

	m_bDistCheckXYOnly = spawnArgs.GetBool( "dist_check_xy", "0" );

	// Add some phase diversity to the checks so that they don't all run in one frame
	// make sure they all run on the first frame though, by initializing m_TimeStamp to
	// be at least one interval early.
	m_DistCheckTimeStamp = gameLocal.time - (int) (m_DistCheckInterval * (1.0f + gameLocal.random.RandomFloat()) );

	// Have to start thinking
	BecomeActive( TH_THINK );
}

/*
===============
Seed::AddSkin - add one skin name to the skins list (if it isn't in there already), and return the index
===============
*/
int Seed::AddSkin( const idStr *skin )
{
	for( int i = 0; i < m_Skins.Num(); i++ )
	{
		if (m_Skins[i] == *skin)
		{
			return i;
		}
	}

	// not yet in list
	m_Skins.Append (*skin);
	return m_Skins.Num() - 1;
}

/*
Seed::ParseFalloff - interpret the falloff spawnarg from the given dictionary
*/
int Seed::ParseFalloff(idDict const *dict, idStr defaultName, idStr defaultFactor, float *func_a) const
{
	int rc = 0;

	idStr falloff = dict->GetString( "seed_falloff", defaultName );
	if (falloff == "none")
	{
		return 0;
	}
	if (falloff == "cutoff")
	{
		return 1;
	}
	if (falloff == "linear")
	{
		return 4;
	}
	if (falloff == "func")
	{
		return 5;
	}

	if (falloff == "power")
	{
		rc = 2;
	}
	if (falloff == "root")
	{
		rc = 3;
	}

	if (rc == 0)
	{
		gameLocal.Warning("SEED %s: Wrong falloff %s, expected one of none, cutoff, power, root, linear or func.\n", GetName(), falloff.c_str() );
		return 0;
	}

	// power or root, store the factor in func_a
	*func_a = dict->GetFloat( "seed_func_a", defaultFactor );
	if (*func_a < 2.0f)
	{
		gameLocal.Warning( "SEED %s: Expect seed_func_a >= 2 when falloff is %s.\n", GetName(), falloff.c_str());
		*func_a = 2.0f;
	}

	return rc;
}

/*
===============
Seed::AddClassFromEntity - take an entity as template and add a class from it. Returns the size of this class
===============
*/
void Seed::AddClassFromEntity( idEntity *ent, const bool watch )
{
	seed_class_t			SeedClass;
	seed_material_t			SeedMaterial;
	const idKeyValue *kv;
	float fImgDensity = 0.0f;		// average "density" of the image map

	// TODO: support for "seed_spawn_probability" (if < 1.0, only spawn entities of this class if RandomFloat() <= seed_spawn_probability)

	SeedClass.classname = ent->GetEntityDefName();
	SeedClass.pseudo = false;		// this is a true entity class
	SeedClass.watch = watch;		// watch over this entity?
	SeedClass.classname = ent->GetEntityDefName();
	SeedClass.modelname = ent->spawnArgs.GetString("model","");

	// is solid?	
	SeedClass.solid = ent->spawnArgs.GetBool("solid","1");

	// can be combined with other entities?
	SeedClass.nocombine = ent->spawnArgs.GetBool("seed_combine","1") ? false : true;

	// never combine these types
	if ( ent->IsType( idMoveable::Type ) ||
		 ent->IsType( CBinaryFrobMover::Type ) ||
		 ent->IsType( idBrittleFracture::Type ) ||
		 ent->IsType( idTarget::Type ) ||
		 ent->IsType( idActor::Type ) ||
		 ent->IsType( idAFEntity_Base::Type ) ||
		 ent->IsType( idAFAttachment::Type ) ||
		 ent->IsType( idAnimatedEntity::Type ) ||
		 ent->IsType( idWeapon::Type ) ||
		 ent->IsType( idLight::Type ) )
	{
		SeedClass.nocombine = true;
	}

	// if can be combined, do some further checks
	if (!SeedClass.nocombine)
	{
    	// never combine entities which have a script object (that won't work)
		idStr scriptobject = ent->spawnArgs.GetString("scriptobject","");
		if (!scriptobject.IsEmpty())
		{
			// gameLocal.Printf("Not combining entities of this class because 'scriptobject' is set.\n");
			SeedClass.nocombine = true;
		}
    	// neither combine entities which have particles as model
		else if (SeedClass.modelname.Right(4) == ".prt")
		{
			// gameLocal.Printf("Not combining entities of this class because model is a particle.\n");
			SeedClass.nocombine = true;
		}
	}

//    // never combine entities which have certain spawnclasses
//	idStr spawnclass = ent->spawnArgs.GetString("spawnclass","");

	// only for pseudo classes
	SeedClass.physicsObj = NULL;

	// debug_colors?
	SeedClass.materialName = "";
	if (m_bDebugColors)
	{
		// select one at random
		SeedClass.materialName = idStr("textures/darkmod/debug/") + seed_debug_materials[ gameLocal.random.RandomInt( SEED_DEBUG_MATERIAL_COUNT ) ];
	}

	SeedClass.score = 0;
	if (!SeedClass.watch)
	{
		// score = 0 for pseudo classes or watch-classes
		SeedClass.score = ent->spawnArgs.GetInt("seed_score","1");
		if (SeedClass.score < 1)
		{
			SeedClass.score = 1;
		}
	}

	// get all "skin" and "skin_xx", as well as "random_skin" spawnargs
	SeedClass.skins.Clear();
	// if no skin spawnarg exists, add the empty skin so we at least have one entry
	if ( ! ent->spawnArgs.FindKey("skin") )
	{
		SeedClass.skins.Append ( 0 );
	}
   	kv = ent->spawnArgs.MatchPrefix( "skin", NULL );
	while( kv )
	{
		// find the proper skin index
		idStr skin = kv->GetValue();
		int skinIdx = AddSkin( &skin );
		gameLocal.Printf( "SEED %s: Adding skin '%s' (idx %i) to class.\n", GetName(), skin.c_str(), skinIdx );
		SeedClass.skins.Append ( skinIdx );
		kv = ent->spawnArgs.MatchPrefix( "skin", kv );
	}
	idStr random_skin = ent->spawnArgs.GetString("random_skin","");
	if ( !random_skin.IsEmpty() )
	{
		gameLocal.Printf( "SEED %s: Entity has random_skin '%s'.\n", GetName(), random_skin.c_str() );
		// split up at "," and add all these to the skins
		// if we have X commata, we have X+1 pieces, so go through all of them
		int start = 0; int end = 0;
		int numChars = random_skin.Length();
		// gameLocal.Printf(" start %i numChars %i\n", start, numChars);
		while (start < numChars)
		{
			// find first non-"," and non " "
			while (start < numChars && (random_skin[start] == ',' ||random_skin[start] == ' ') )
			{
				start++;
			}
			if (start < numChars)
			{
				// have at least one non ','
				end = start + 1;
				while (end < numChars && random_skin[end] != ',')
				{
					end++;
				}
				// cut between start and end
				if (end - start > 0)
				{
					idStr skin = random_skin.Mid(start, end - start);
					// "''" => "" (default skin)
					if (skin == "''")
					{
						skin = "";
					}
					int skinIdx = AddSkin( &skin );
					gameLocal.Printf( "SEED %s: Adding random skin '%s' (idx %i) to class.\n", GetName(), skin.c_str(), skinIdx );
					SeedClass.skins.Append ( skinIdx );
				}
				start = end;
				// next part
			}
		}
	}

	// Do not use GetPhysics()->GetOrigin(), as the LOD system might have shifted
	// the entity already between spawning and us querying the info:
	SeedClass.origin = ent->spawnArgs.GetVector( "origin" );

	// add "seed_offset" to correct for mismatched origins
	SeedClass.offset = ent->spawnArgs.GetVector( "seed_offset", "0 0 0" );

	// these are ignored for pseudo classes (e.g. watch_breathren):
	SeedClass.floor = ent->spawnArgs.GetBool( "seed_floor", spawnArgs.GetString( "floor", "0") );
	SeedClass.stack = ent->spawnArgs.GetBool( "seed_stack", "1" );
	SeedClass.noinhibit = ent->spawnArgs.GetBool( "seed_noinhibit", "0" );

	SeedClass.spacing = ent->spawnArgs.GetFloat( "seed_spacing", "0" );

	// to randomly sink entities into the floor
	SeedClass.sink_min = ent->spawnArgs.GetFloat( "seed_sink_min", spawnArgs.GetString( "sink_min", "0") );
	SeedClass.sink_max = ent->spawnArgs.GetFloat( "seed_sink_max", spawnArgs.GetString( "sink_max", "0") );
	if (SeedClass.sink_max < SeedClass.sink_min) { SeedClass.sink_max = SeedClass.sink_min; }

	// to support scaling of all axes with the same value, peek into seed_scale_min and seed_scale_max
	idStr scale_min = ent->spawnArgs.GetString( "seed_scale_min", spawnArgs.GetString( "scale_min", "1 1 1") );
	idStr scale_max = ent->spawnArgs.GetString( "seed_scale_max", spawnArgs.GetString( "scale_max", "1 1 1") );
	if (scale_min.Find(' ') < 0)
	{
		// set x and y to 0 to signal code to use axes-equal scaling
		SeedClass.scale_min = idVec3( 0, 0, ent->spawnArgs.GetFloat( "seed_scale_min", spawnArgs.GetString( "scale_min", "1") ) );
	}
	else
	{
		SeedClass.scale_min = ent->spawnArgs.GetVector( "seed_scale_min", spawnArgs.GetString( "scale_min", "1 1 1") );
	}

	if (scale_max.Find(' ') < 0)
	{
		// set x and y to 0 to signal code to use axes-equal scaling
		SeedClass.scale_max = idVec3( 0, 0, ent->spawnArgs.GetFloat( "seed_scale_max", spawnArgs.GetString( "scale_max", "1") ) );
	}
	else
	{
		SeedClass.scale_max = ent->spawnArgs.GetVector( "seed_scale_max", spawnArgs.GetString( "scale_max", "1 1 1") );
	}

	if (SeedClass.scale_max.x < SeedClass.scale_min.x) { SeedClass.scale_max.x = SeedClass.scale_min.x; }
	if (SeedClass.scale_max.y < SeedClass.scale_min.y) { SeedClass.scale_max.y = SeedClass.scale_min.y; }
	if (SeedClass.scale_max.z < SeedClass.scale_min.z) { SeedClass.scale_max.z = SeedClass.scale_min.z; }

	SeedClass.func_x = 0;
	SeedClass.func_y = 0;
	SeedClass.func_s = 0;
	SeedClass.func_a = 0;
	SeedClass.func_Xt = 0;
	SeedClass.func_Yt = 0;
	SeedClass.func_f = 0;

	SeedClass.falloff = ParseFalloff( &ent->spawnArgs, spawnArgs.GetString( "falloff", "none"), spawnArgs.GetString( "func_a", "2"), &SeedClass.func_a);
	// falloff == func
	if (SeedClass.falloff == 5)
	{
		// default is 0.5 * (x + y + 0)
		SeedClass.func_a = ent->spawnArgs.GetFloat( "seed_func_a", spawnArgs.GetString( "func_a", "0") );
		SeedClass.func_s = ent->spawnArgs.GetFloat( "seed_func_s", spawnArgs.GetString( "func_s", "0.5") );
		SeedClass.func_Xt = 1;			// 1 - X, 2 -> X * X
		idStr x = ent->spawnArgs.GetString( "seed_func_Xt", spawnArgs.GetString( "func_Xt", "X") );
		if (x == "X*X")
		{
			SeedClass.func_Xt = 2;		// 1 - X, 2 -> X * X
		}
		SeedClass.func_Yt = 1;			// 1 - X, 2 -> X * X
		x = ent->spawnArgs.GetString( "seed_func_Yt", spawnArgs.GetString( "func_Yt", "Y") );
		if (x == "Y*Y")
		{
			SeedClass.func_Yt = 2;		// 1 - Y, 2 -> Y * Y
		}
		SeedClass.func_x = ent->spawnArgs.GetFloat( "seed_func_x", spawnArgs.GetString( "func_x", "1") );
		SeedClass.func_y = ent->spawnArgs.GetFloat( "seed_func_y", spawnArgs.GetString( "func_y", "1") );
		SeedClass.func_min = ent->spawnArgs.GetFloat( "seed_func_min", spawnArgs.GetString( "func_min", "0") );
		SeedClass.func_max = ent->spawnArgs.GetFloat( "seed_func_max", spawnArgs.GetString( "func_max", "1.0") );
		if (SeedClass.func_min < 0.0f)
		{
			gameLocal.Warning ("SEED %s: func_min %0.2f < 0, setting it to 0.\n", GetName(), SeedClass.func_min );
			SeedClass.func_min = 0.0f;
		}
		if (SeedClass.func_max > 1.0f)
		{
			gameLocal.Warning ("SEED %s: func_max %0.2f < 1.0, setting it to 1.0.\n", GetName(), SeedClass.func_max );
			SeedClass.func_max = 1.0f;
		}
		if (SeedClass.func_min > SeedClass.func_max)
		{
			gameLocal.Warning ("SEED %s: func_min %0.2f > func_max %0.2f, setting it to 0.\n", GetName(), SeedClass.func_min, SeedClass.func_max );
			SeedClass.func_min = 0.0f;
		}

		x = ent->spawnArgs.GetString( "seed_func_f", spawnArgs.GetString( "func_f", "clamp") );
		if (x == "clamp")
		{
			SeedClass.func_f = 1;
		}
		else if (x != "zeroclamp")
		{
			gameLocal.Error ("SEED %s: func_clamp is invalid, expected 'clamp' or 'zeroclamp', found '%s'\n", GetName(), x.c_str() );
		}
		gameLocal.Warning ("SEED %s: Using falloff func p = %s( %0.2f, %0.2f, %0.2f * ( %s * %0.2f + %s * %0.2f + %0.2f) )\n", 
				GetName(), x.c_str(), SeedClass.func_min, SeedClass.func_max, SeedClass.func_s, SeedClass.func_Xt == 1 ? "X" : "X*X", SeedClass.func_x, 
				SeedClass.func_Yt == 1 ? "Y" : "Y*Y", SeedClass.func_y, SeedClass.func_a );
	}

	// image based map?
	idStr mapName = ent->spawnArgs.GetString( "seed_map", spawnArgs.GetString( "map", "") );

	SeedClass.imgmap = 0;
	if (!mapName.IsEmpty())
	{
		SeedClass.imgmap = gameLocal.m_ImageMapManager->GetImageMap( mapName );
		if (SeedClass.imgmap == 0)
		{
			gameLocal.Warning ("SEED %s: Could not load image map mapName: %s", GetName(), gameLocal.m_ImageMapManager->GetLastError() );
		}
	}
	SeedClass.map_invert = false;
	SeedClass.map_scale_x = 1.0f;
	SeedClass.map_scale_y = 1.0f;
	SeedClass.map_ofs_x = 0.0f;
	SeedClass.map_ofs_y = 0.0f;
	// not empty => image based map
	if ( SeedClass.imgmap > 0)
	{
	    SeedClass.map_invert = ent->spawnArgs.GetBool( "seed_map_invert", spawnArgs.GetString( "map_invert", "0") );

		SeedClass.map_scale_x = 
			ent->spawnArgs.GetFloat( "seed_map_scale_x", 
					ent->spawnArgs.GetString( "seed_map_scale",			// if seed_map_scale_x is not set, try "seed_map_scale"
					   	spawnArgs.GetString( "map_scale_x",				// and if that isn't set either, try map_scale_x
						   	spawnArgs.GetString( "map_scale",			// and if that isn't set either, try map_scale
						   	"1.0" ) ) ) );								// finally fallback to 1.0
		SeedClass.map_scale_y = 
			ent->spawnArgs.GetFloat( "seed_map_scale_y", 
					ent->spawnArgs.GetString( "seed_map_scale",			// if seed_map_scale_y is not set, try "seed_map_scale"
					   	spawnArgs.GetString( "map_scale_y",				// and if that isn't set either, try SEED::map_scale_y
						   	spawnArgs.GetString( "map_scale",			// and if that isn't set either, try SEED::map_scale
						   	"1.0" ) ) ) );								// finally fallback to 1.0
		SeedClass.map_ofs_x = 
			ent->spawnArgs.GetFloat( "seed_map_ofs_x", 
					ent->spawnArgs.GetString( "seed_map_ofs",			// if seed_map_scale_x is not set, try "seed_map_ofs"
					   	spawnArgs.GetString( "map_ofs_x",				// and if that isn't set either, try SEED::map_scale_x
						   	spawnArgs.GetString( "map_ofs",				// and if that isn't set either, try SEED::map_scale
						   	"0" ) ) ) );								// finally fallback to 0
		SeedClass.map_ofs_y = 
			ent->spawnArgs.GetFloat( "seed_map_ofs_y", 
					ent->spawnArgs.GetString( "seed_map_ofs",			// if seed_map_scale_y is not set, try "seed_map_ofs"
					   	spawnArgs.GetString( "map_ofs_y",				// and if that isn't set either, try SEED::map_scale_y
						   	spawnArgs.GetString( "map_ofs",				// and if that isn't set either, try SEED::map_scale
						   	"0" ) ) ) );								// finally fallback to 0

		unsigned char *imgData = gameLocal.m_ImageMapManager->GetMapData( SeedClass.imgmap );
		if (!imgData)
		{
			gameLocal.Error("SEED %s: Could not access image data from %s.\n", 
					GetName(), gameLocal.m_ImageMapManager->GetMapName( SeedClass.imgmap ) );
		}

		unsigned int bpp = gameLocal.m_ImageMapManager->GetMapBpp( SeedClass.imgmap );
		if (bpp != 1)
		{
			gameLocal.Error("SEED %s: Bytes per pixel must be 1 but is %i!\n", GetName(), bpp );
		}

		// Compute an average density for the image map, so we can correct the number of entities
		// based on this. An image map with 50% black and 50% white should result in 0.5, as should 50% grey:
		unsigned int w = gameLocal.m_ImageMapManager->GetMapWidth( SeedClass.imgmap );
		unsigned int h = gameLocal.m_ImageMapManager->GetMapHeight( SeedClass.imgmap );
		if (SeedClass.map_ofs_x == 0 && SeedClass.map_ofs_y == 0 && SeedClass.map_scale_x == 1.0f && SeedClass.map_scale_y == 1.0f)
		{
			// can use the precomputed density of the entire image map
			fImgDensity = gameLocal.m_ImageMapManager->GetMapDensity( SeedClass.imgmap );
		}
		else
		{
			double wd = (double)w;
			double hd = (double)h;
			double xo = w * SeedClass.map_ofs_x;
			double yo = h * SeedClass.map_ofs_y;
			double xs = SeedClass.map_scale_x;
			double ys = SeedClass.map_scale_y;
			for (unsigned int x = 0; x < w; x++)
			{
				for (unsigned int y = 0; y < h; y++)
				{
					// compute X and Y based on scaling/offset
					// first fmod => -w .. +w => +w => 0 .. 2 * w => fmod => 0 .. w
					int x1 = fmod( fmod( x * xs + xo, wd ) + wd, wd);
					int y1 = fmod( fmod( y * ys + yo, hd ) + hd, hd);
					fImgDensity += (float)imgData[ w * y1 + x1 ];	// 0 .. 255
				}
			}
			// divide the sum by W and H and 256 so we arrive at 0 .. 1.0
			fImgDensity /= (float)(w * h * 256.0f);
		}

		// if the map is inverted, use 1 - x:
		if (SeedClass.map_invert)
		{
			fImgDensity = 1 - fImgDensity;
		}

		gameLocal.Printf("SEED %s: Using %s: %ix%i px, %i bpp, average density %0.4f.\n", 
				GetName(), gameLocal.m_ImageMapManager->GetMapName( SeedClass.imgmap ) ,
			   	w, h, bpp, fImgDensity );
		if (fImgDensity < 0.001f)
		{
			gameLocal.Warning("The average density of this image map is very low.");
			// avoid divide-by-zero
			fImgDensity = 0.001f;
		}
	}

	SeedClass.bunching = ent->spawnArgs.GetFloat( "seed_bunching", spawnArgs.GetString( "bunching", "0") );
	if (SeedClass.bunching < 0 || SeedClass.bunching > 1.0)
	{
		gameLocal.Warning ("SEED %s: Invalid bunching value %0.2f, must be between 0 and 1.0.\n", GetName(), SeedClass.bunching );
		SeedClass.bunching = 0;
	}
	if (SeedClass.spacing > 0)
	{
		SeedClass.nocollide	= NOCOLLIDE_ATALL;
	}
	else
	{
		// don't collide with other existing statics, but collide with the autogenerated ones
		// TODO: parse from "seed_nocollide" "same, other, world, static"
		SeedClass.nocollide	= NOCOLLIDE_STATIC;
	}
	// set rotation of entity to 0, so we get the unrotated bounds size
	ent->SetAxis( mat3_identity );

	// TODO: in case the entity is non-solid, this ends up as 0x0, try to find the size.
	SeedClass.size = ent->GetRenderEntity()->bounds.GetSize();

	// in case the size is something like 8x0 (a single flat poly) or 0x0 (no clipmodel):
	float fMin = 1.0f;
	if (SeedClass.size.x < 0.001f)
	{
		gameLocal.Warning( "SEED %s: Size.x < 0.001 for class, enforcing minimum size %0.2f.\n", GetName(), fMin );
		SeedClass.size.x = fMin;
	}
	if (SeedClass.size.y < 0.001f)
	{
		gameLocal.Warning( "SEED %s: Size.y < 0.001 for class, enforcing minimum size %0.2f.\n", GetName(), fMin );
		SeedClass.size.y = fMin;
	}

	// gameLocal.Printf( "SEED %s: size of class %i: %0.2f %0.2f\n", GetName(), i, SeedClass.size.x, SeedClass.size.y );
	// TODO: use a projection along the "floor-normal"

	// gameLocal.Printf( "SEED %s: Entity class size %0.2f %0.2f %0.2f\n", GetName(), SeedClass.size.x, SeedClass.size.y, SeedClass.size.z );
	SeedClass.cullDist = 0;
	SeedClass.spawnDist = 0;
	float hideDist = ent->spawnArgs.GetFloat( "hide_distance", "0" );
	float cullRange = ent->spawnArgs.GetFloat( "seed_cull_range", spawnArgs.GetString( "cull_range", "150" ) );
	if (cullRange > 0 && hideDist > 0)
	{
		SeedClass.cullDist = hideDist + cullRange;
		SeedClass.spawnDist = hideDist + (cullRange / 2);
		// square for easier compare
		SeedClass.cullDist *= SeedClass.cullDist;
		SeedClass.spawnDist *= SeedClass.spawnDist;
	}

	const idDict* dict = gameLocal.FindEntityDefDict( SeedClass.classname );

	if (!dict)
	{
		gameLocal.Error("SEED %s: Error, cannot find entity def dict for %s.\n", GetName(), SeedClass.classname.c_str() );
	}

	// parse the spawnargs from this entity def for LOD data, and ignore any hide_probability:
	float has_lod = ParseLODSpawnargs( dict, 1.0f );

	if (has_lod)
	{
		// Store m_LOD at the class
		SeedClass.m_LOD = m_LOD;
	}
	else
	{
		SeedClass.m_LOD = NULL;
	}
	m_LOD = NULL;				// prevent double free (and SEED doesn't have LOD)
	SeedClass.materials.Clear();

	// The default probability for all materials not matching anything in materials:
	SeedClass.defaultProb = ent->spawnArgs.GetFloat( "seed_probability", spawnArgs.GetString( "probability", "1.0" ) );

	// all probabilities for the different materials
	kv = ent->spawnArgs.MatchPrefix( "seed_material_", NULL );
	while( kv )
   	{
		// "seed_material_grass" => "grass"
		SeedMaterial.name = kv->GetKey().Mid( 14, kv->GetKey().Length() - 14 );
		// "seed_material_grass" "1.0" => 1.0
		SeedMaterial.probability = ent->spawnArgs.GetFloat( kv->GetKey(), "1.0");
		if (SeedMaterial.probability < 0 || SeedMaterial.probability > 1.0)
		{
			gameLocal.Warning( "SEED %s: Invalid probability %0.2f (should be 0 .. 1.0) for material %s, ignoring it.\n",
					GetName(), SeedMaterial.probability, SeedMaterial.name.c_str() );
		}
		else
		{
			//gameLocal.Warning( "SEED %s: Using material %s, probability %0.2f (%s)\n",
			//		GetName(), SeedMaterial.name.c_str(), SeedMaterial.probability, kv->GetKey().c_str() );
			SeedClass.materials.Append( SeedMaterial );
		}
		kv = ent->spawnArgs.MatchPrefix( "seed_material_", kv );
	}

	// store the rendermodel to make func_statics or scaling/combining work
	SeedClass.hModel = NULL;
	SeedClass.clip = NULL;
	if (SeedClass.classname == FUNC_STATIC)
	{
		// check if this is a func_static with a model, or an "inline map geometry" func static
		if (SeedClass.modelname == ent->GetName())
		{
			// simply point to the already existing model, so we can recover the into-the-map-inlined geometry:
			SeedClass.hModel = ent->GetRenderEntity()->hModel;
			// prevent a double free
			ent->GetRenderEntity()->hModel = NULL;
			// set a dummy model
			SeedClass.modelname = "models/darkmod/junk/plank_short.lwo";

			// store a copy of the clipmodel, so we can later reuse it
			SeedClass.clip = new idClipModel( ent->GetPhysics()->GetClipModel() );
#ifdef M_DEBUG
			gameLocal.Printf("Using clip from rendermodel ptr=0x%p bounds %s\n", SeedClass.clip, SeedClass.clip->GetBounds().ToString());
#endif
			SeedClass.classname = FUNC_DUMMY;
		}
		else
		{
			// Only use the CStaticMulti class if we are going to combine things, otherwise leave it as "func_static"
			if ( m_bCombine )
			{
				SeedClass.classname = FUNC_DUMMY;
			}
			// if we are not combining things, but scale, set hModel so it later gets duplicated
			else
			{
				// if scale_min.x == 0, axis-equal scaling
			   	if (SeedClass.scale_max.z != 1.0f || SeedClass.scale_min.z != 1.0f ||
			   		(SeedClass.scale_min.x != 0.0f && SeedClass.scale_min.x != 1.0f) ||
			   		(SeedClass.scale_max.x != 1.0f || SeedClass.scale_min.y != 1.0f || SeedClass.scale_max.y != 1.0f) )
				{
				// simply point to the already existing model, so we can clone it later
				SeedClass.hModel = ent->GetRenderEntity()->hModel;
				}
			}
		}
	}

	// uses color variance?
	// fall back to SEED "color_mxx", if not set, fall back to entity color, if this is unset, use 1 1 1
	SeedClass.color_min  = ent->spawnArgs.GetVector("seed_color_min", spawnArgs.GetString("color_min", ent->spawnArgs.GetString("_color", "1 1 1")));
	SeedClass.color_max  = ent->spawnArgs.GetVector("seed_color_max", spawnArgs.GetString("color_max", ent->spawnArgs.GetString("_color", "1 1 1")));

    SeedClass.color_min.Clamp( idVec3(0,0,0), idVec3(1,1,1) );
    SeedClass.color_max.Clamp( SeedClass.color_min, idVec3(1,1,1) );

	// apply random impulse?
	// fall back to SEED "impulse_mxx", if not set, use 0 0 0
	SeedClass.impulse_min  = ent->spawnArgs.GetVector("seed_impulse_min", spawnArgs.GetString("impulse_min", "0 -90 0"));
	SeedClass.impulse_max  = ent->spawnArgs.GetVector("seed_impulse_max", spawnArgs.GetString("impulse_max", "0 90 360"));

	// clamp to 0..360, -180..180, 0..1000
    SeedClass.impulse_min.Clamp( idVec3(0,-90,0), idVec3(1000,+90,359.9f) );
    SeedClass.impulse_max.Clamp( SeedClass.impulse_min, idVec3(1000,+90,360) );

    SeedClass.z_invert = ent->spawnArgs.GetBool("seed_z_invert", spawnArgs.GetString("z_invert", "0"));
    SeedClass.z_min = ent->spawnArgs.GetFloat("seed_z_min", spawnArgs.GetString("z_min", "-1000000"));
    SeedClass.z_max = ent->spawnArgs.GetFloat("seed_z_max", spawnArgs.GetString("z_max", "1000000"));

	if (SeedClass.z_max < SeedClass.z_max)
	{
		// hm, should we warn?
		SeedClass.z_max = SeedClass.z_min;
	}
    SeedClass.z_fadein  = ent->spawnArgs.GetFloat("seed_z_fadein",  spawnArgs.GetString("z_fadein", "0"));
    SeedClass.z_fadeout = ent->spawnArgs.GetFloat("seed_z_fadeout", spawnArgs.GetString("z_fadeout", "0"));
	if (SeedClass.z_fadein < 0)
	{
		gameLocal.Warning( "SEED %s: Invalid z-fadein %0.2f (should be >= 0) for class %s, ignoring it.\n",
				GetName(), SeedClass.z_fadein, SeedClass.classname.c_str() );
		SeedClass.z_fadein = 0;
	}
	if (SeedClass.z_fadeout < 0)
	{
		gameLocal.Warning( "SEED %s: Invalid z-fadeout %0.2f (should be >= 0) for class %s, ignoring it.\n",
				GetName(), SeedClass.z_fadeout, SeedClass.classname.c_str() );
		SeedClass.z_fadeout = 0;
	}
	
	if (SeedClass.z_min + SeedClass.z_fadein > SeedClass.z_max - SeedClass.z_fadeout)
	{
		// hm, should we warn?
	    SeedClass.z_fadein = SeedClass.z_max - SeedClass.z_fadeout - SeedClass.z_min;
	}

	if (SeedClass.z_min != -1000000 && !SeedClass.floor)
	{
		gameLocal.Warning( "SEED %s: Warning: Setting seed_z_min/seed_z_max without setting 'seed_floor' to true won't work!\n", GetName() );
		// just use flooring for this class
		SeedClass.floor = true;
	}
	gameLocal.Printf( "SEED %s: Adding class %s.\n", GetName(), SeedClass.classname.c_str() );

	// if the size on x or y is very small, use "0.2" instead. This is to avoid that models
	// consisting of a flat plane get 0 as size:
//	gameLocal.Printf( "SEED %s: size %0.2f x %0.2f.\n", GetName(), fmax( 0.1, SeedClass.size.x), fmax( 0.1, SeedClass.size.y ) );

	float size = (std::max(0.1f, SeedClass.size.x) + SeedClass.spacing) * (std::max( 0.1f, SeedClass.size.y) + SeedClass.spacing);

	// if falloff != none, correct the density, because the ellipse-shape is smaller then the rectangle
	if ( SeedClass.falloff >= 1 && SeedClass.falloff <= 3)
	{
		// Rectangle is W * H, ellipse is W/2 * H/2 * PI. When W = H = 1, then the rectangle
		// area is 1.0, and the ellipse 0.785398, so correct for 1 / 0.785398 = 1.2732, this will
		// reduce the density, and thus the entity count:
		size *= 4 / idMath::PI; 
	}
	// TODO: take into account inhibitors (these should reduce the density)

	// minimum density values
	fMin = 0.000001f;

	// scale the per-class size by the per-class density
	float fDensity = std::max( fMin, ent->spawnArgs.GetFloat( "seed_density", "1.0" ) );
	// scale the per-class size by the per-class density multiplied by the base density
	float fBaseDensity = std::max( fMin, ent->spawnArgs.GetFloat( "seed_base_density", "1.0" ) );

//	gameLocal.Printf( "SEED %s: Scaling class size by %0.4f (%0.6f * %0.6f).\n", GetName(), fDensity*fBaseDensity, fDensity, fBaseDensity);
//	gameLocal.Printf( "SEED %s: size = %0.6f fImgDensity = %0.6f\n", GetName(), size, fImgDensity );

	// Simple reduce the size if the density should increase
	SeedClass.avgSize = size / ( fBaseDensity * fImgDensity * fDensity );

	// if the mapper wants a hard limit on this class
	SeedClass.maxEntities = spawnArgs.GetInt( "seed_max_entities", "0" );
	SeedClass.numEntities = 0;

	// all data setup, append to the list
	m_Classes.Append ( SeedClass );

	return;
}

/*
===============
Generate a scaling factor depending on the GUI setting
===============
*/
float Seed::LODBIAS ( void ) const
{
	// scale density with GUI setting
	// The GUI specifies: 0.5;0.75;1.0;1.5;2.0;3.0, but 0.5 and 3.0 are quite extreme,
	// so we scale the values first:
	float lod_bias = cv_lod_bias.GetFloat();
	if (lod_bias < 0.8)
	{
		if (lod_bias < 0.7)
		{
			lod_bias *= 1.4f;									// 0.5 => 0.7
		}
		else
		{
			lod_bias *= 1.2f;									// 0.75 => 0.90
		}
	}
	else if (lod_bias > 1.0f)
	{
																// 1.5, 2, 3 => 1.13, 1.25, 1.4
		lod_bias = ( lod_bias > 2.0f ? 0.9f : 1.0f) + ((lod_bias - 1.0f) / 4.0f);
	}

	// 0.7, 0.9, 1.0, 1.13, 1.25, 1.4
	return lod_bias;
}

/*
===============
Compute the max. number of entities that we manage
===============
*/
void Seed::ComputeEntityCount( void )
{
	// compute entity count dynamically from area that we cover
	float fDensity = spawnArgs.GetFloat( "density", "1.0" );

	// Scaled by GUI setting?
	if (spawnArgs.GetBool( "lod_scale_density", "1"))
	{
		fDensity *= LODBIAS();
	}

	fDensity = std::max( fDensity, 0.00001f);		// at minimum 0.00001f

	idBounds bounds = renderEntity.bounds;
	idVec3 size = bounds.GetSize();

	float fArea = (size.x + 1) * (size.y + 1);
//	gameLocal.Printf( "SEED %s: Area: %0.2f fDensity %0.4f\n", GetName(), fArea, fDensity);
	fArea *= fDensity;

	int n = m_Classes.Num();

	// compute the overall score
	int iScoreSum = 0;

	// limit the overall entity count?
	int max_entities = spawnArgs.GetInt( "max_entities", "0" );
   	if (max_entities > 0)
	{
	   	if (max_entities > spawnArgs.GetFloat( "lod_scaling_limit", "10" ))
		{
			max_entities *= LODBIAS();
		}
		for( int i = 0; i < n; i++ )
		{
			// pseudo classes and watch-over-breathren have score == 0
			iScoreSum += m_Classes[i].score;
		}
	}

	// sum the entities for each class together (otherwise we would compute the average entity count
	// over all classes):
	int numRealClasses = 0;

	m_iNumEntities = 0;
	for( int i = 0; i < n; i++ )
	{
		// ignore pseudo classes and watch-over-breathren
		if (m_Classes[i].pseudo || m_Classes[i].watch)
		{
			continue;
		}
		numRealClasses ++;

		int newNum = 0;
		if (max_entities > 0)
		{
			// max entities is set on the SEED, so use the score to calculate the entities for each class
			newNum = std::max( 1, (max_entities * m_Classes[i].score) / iScoreSum );	// at least one from each class
		}
		else
		{
			if ( m_Classes[i].avgSize > 0)
			{
				newNum = fArea / m_Classes[i].avgSize;
			}
//			gameLocal.Printf( "SEED %s: Dynamic entity count: %i += %i ( fArea = %0.4f, fAvgSize = %0.4f).\n", 
//					GetName(), m_iNumEntities, newNum, fArea, m_Classes[i].avgSize );
		}

		if (m_Classes[i].maxEntities > 0 && newNum > m_Classes[i].maxEntities)
		{ 
			newNum = m_Classes[i].maxEntities;
		}

		m_Classes[i].numEntities = newNum;
		m_iNumEntities += newNum;
	}

	if (max_entities > 0)
	{
		// limit the overall count to max_entities, even if all classes together have more, to make
		// the "1 out of 4 classes" work:
		m_iNumEntities = max_entities;
	}

	gameLocal.Printf( "SEED %s: Entity count: %i.\n", GetName(), m_iNumEntities );

	// We do no longer impose a limit, as no more than SPAWN_LIMIT will be existing:
	/* if (m_iNumEntities > SPAWN_LIMIT)
	{
		m_iNumEntities = SPAWN_LIMIT;
	}
	*/
}

/*
===============
Create the places for all entities that we control so we can later spawn them.
===============
*/
void Seed::Prepare( void )
{	
	seed_inhibitor_t SeedInhibitor;

	// Gather all targets and make a note of them
	m_Classes.Clear();
	m_Inhibitors.Clear();

	for( int i = 0; i < targets.Num(); i++ )
	{
		idEntity *ent = targets[ i ].GetEntity();

	   	if ( ent )
		{
			// if this is a SEED inhibitor, add it to our "forbidden zones":
			if ( idStr( ent->GetEntityDefName() ) == "atdm:no_seed")
			{
				idBounds b = ent->GetRenderEntity()->bounds; 
				SeedInhibitor.size = b.GetSize();
				gameLocal.Printf( "SEED %s: Inhibitor size %s\n", GetName(), SeedInhibitor.size.ToString() );

				SeedInhibitor.origin = ent->spawnArgs.GetVector( "origin" );
				// the "axis" part does not work, as DR simply rotates the brush model, but does not record an axis
				// or rotation spawnarg. Use clipmodel instead? Note: Unrotating the entity, but adding an "axis"
				// spawnarg works.
				SeedInhibitor.box = idBox( SeedInhibitor.origin, SeedInhibitor.size / 2, ent->GetPhysics()->GetAxis() );

				SeedInhibitor.falloff = ParseFalloff( &ent->spawnArgs, ent->spawnArgs.GetString( "falloff", "none"), ent->spawnArgs.GetString( "func_a", "2"), &SeedInhibitor.factor );
				if (SeedInhibitor.falloff > 4)
				{
					// func is not supported
					gameLocal.Warning( "SEED %s: falloff=func not yet supported on inhibitors, ignoring it.\n", GetName() );
					SeedInhibitor.falloff = 0;
				}

				// default is "noinhibit" (and this will be ignored if classnames.Num() == 0)
				SeedInhibitor.inhibit_only = false;
				SeedInhibitor.classnames.Clear();

				const idKeyValue *kv;
				idStr prefix = "inhibit";

				// if "inhibit" is set, it will only inhibit the given classes, and we ignore "noinhibit":
				if ( ent->spawnArgs.FindKey(prefix) )
				{
					SeedInhibitor.inhibit_only = true;
				}
				else
				{
					prefix = "noinhibit";
					if (! ent->spawnArgs.FindKey(prefix) )
					{
						// SeedInhibitor.inhibit_only = true;		// will be ignored anyway
						prefix = "";
					}

				}

				// have either inhibit or noinhibit in the spawnargs?
				if ( !prefix.IsEmpty())
				{
					gameLocal.Printf( "SEED %s: Inhibitor has %s set.\n", GetName(), prefix.c_str() );
   					kv = ent->spawnArgs.MatchPrefix( prefix, NULL );
					while( kv )
					{
						idStr classname = kv->GetValue();
						gameLocal.Printf( "SEED %s: Inhibitor adding class '%s' (%s)\n", GetName(), classname.c_str(), SeedInhibitor.inhibit_only ? "inhibit" : "noinhibit" );
						SeedInhibitor.classnames.Append( classname );
						// next one please
						kv = ent->spawnArgs.MatchPrefix( prefix, kv );
					}
				}

				m_Inhibitors.Append ( SeedInhibitor );
				continue;
			}

			// If this entity wants us to watch over his brethren, add them to our list:
			if ( ent->spawnArgs.GetBool( "seed_watch_brethren", "0" ) )
			{
				gameLocal.Printf( "SEED %s: %s (%s) wants us to take care of his brethren.\n",
						GetName(), ent->GetName(), ent->GetEntityDefName() );

				// add a pseudo class and ignore the size returned
				AddClassFromEntity( ent, true );

				// no more to do for this target
				continue;
			}

			// add a class based on this entity
			AddClassFromEntity( ent );
		}
	}

	// the same, but this time for the "spawn_class/spawn_count/spawn_skin" spawnargs:
	
	idVec3 origin = GetPhysics()->GetOrigin();

	const idKeyValue *kv = spawnArgs.MatchPrefix( "spawn_class", NULL );
	while( kv )
	{
		idStr entityClass = kv->GetValue();

		// spawn an entity of this class so we can copy it's values
		// TODO: avoid the spawn for speed reasons?

		const char* pstr_DefName = entityClass.c_str();
		const idDict *p_Def = gameLocal.FindEntityDefDict( pstr_DefName, false );
		if( p_Def )
		{
			idEntity *ent;
			idDict args;

			args.Set("classname", entityClass);
			// move to origin of ourselfs
			args.SetVector("origin", origin);

			// want it floored
			args.Set("seed_floor", "1");

			// but if it is a moveable, don't floor it
			args.Set("floor", "0");

			// set previously defined (possible random) skin
			// spawn_classX => spawn_skinX
			idStr skin = idStr("spawn_skin") + kv->GetKey().Mid( 11, kv->GetKey().Length() - 11 );

			// spawn_classX => "abc, def, '', abc"
			skin = spawnArgs.GetString( skin, "" );
			// select one at random
			skin = skin.RandomPart();

			//gameLocal.Printf("Using random skin '%s'.\n", skin.c_str() );
			args.Set( "skin", skin );

			// TODO: if the entity contains a "random_skin", too, use the info from there, then remove it
			args.Set( "random_skin", "" );

			gameLocal.SpawnEntityDef( args, &ent );
			if (ent)
			{
				// add a class based on this entity
				AddClassFromEntity( ent );
				// remove the temp. entity 
				ent->PostEventMS( &EV_Remove, 0 );
			}
			else
			{
				gameLocal.Warning("SEED %s: Could not spawn entity from class %s to add it as my target.\n", 
						GetName(), entityClass.c_str() );
			}
		}
		else
		{
				gameLocal.Warning("SEED %s: Could not find entity def for class %s to add it as my target.\n", 
						GetName(), entityClass.c_str() );
		}

		// next one please
		kv = spawnArgs.MatchPrefix( "spawn_class", kv );
	}

	if (m_Classes.Num() > 0)
	{
		// set m_iNumEntities from spawnarg, or density, taking GUI setting into account
		ComputeEntityCount();

		if (m_iNumEntities <= 0)
		{
			gameLocal.Warning( "SEED %s: entity count is invalid: %i!\n", GetName(), m_iNumEntities );
			m_iNumEntities = 0;
		}
	}

	gameLocal.Printf( "SEED %s: Max. entity count: %i\n", GetName(), m_iNumEntities );

	// Init the seed. 0 means random sequence, otherwise use the specified value
    // so that we get exactly the same sequence every time:
	m_iSeed_2 = spawnArgs.GetInt( "randseed", "0" );
    if (m_iSeed_2 == 0)
	{
		// The randseed upon loading a map seems to be always 0, so 
		// gameLocal.random.RandomInt() always returns 1 hence it is unusable:
		// add the entity number so that different seeds spawned in the same second
		// don't display the same pattern
		unsigned long seconds = (unsigned long) time (NULL) + (unsigned long) entityNumber;
	    m_iSeed_2 = (int) (1664525L * seconds + 1013904223L) & 0x7FFFFFFFL;
	}

	// to restart the same sequence, f.i. when the user changes level of detail in GUI
	m_iOrgSeed = m_iSeed_2;

	PrepareEntities();

	// remove all our targets from the game
	for( int i = 0; i < targets.Num(); i++ )
	{
		idEntity *ent = targets[ i ].GetEntity();
		if (ent)
		{
			ent->PostEventMS( &EV_SafeRemove, 0 );
		}
	}
	targets.Clear();

	// Remove ourself after spawn? But not if we have registered entities, these
	// need our service upon Restore().
	if (spawnArgs.GetBool("remove","0"))
	{
		if (m_iNumStaticMulties > 0)
		{
			gameLocal.Printf( "SEED %s: Cannot remove myself, because I have %i static multies.\n", GetName(), m_iNumStaticMulties );
		}
		else
		{
			// spawn all entities
			gameLocal.Printf( "SEED %s: Spawning all %i entities and then removing myself.\n", GetName(), m_iNumEntities );

			// for each of our "entities", do the distance check
			for (int i = 0; i < m_Entities.Num(); i++)
			{
				SpawnEntity(i, false);		// spawn as unmanaged
			}

			// clear out memory just to be sure
			ClearClasses();
			m_Entities.Clear();
			m_iNumEntities = -1;

			active = false;
			BecomeInactive( TH_THINK );

			// post event to remove ourselfes
			PostEventMS( &EV_SafeRemove, 0 );
		}
	}
	else
	{
		m_bPrepared = true;
		if (m_Entities.Num() == 0)
		{
			// could not create any entities?
			gameLocal.Printf( "SEED %s: Have no entities to control, becoming inactive.\n", GetName() );
			// Tels: Does somehow not work, bouncing us again and again into this branch?
			BecomeInactive(TH_THINK);
			m_iNumEntities = -1;
		}
	}
}

void Seed::PrepareEntities( void )
{
	seed_entity_t			SeedEntity;			// temp. storage
	idBounds				testBounds;			// to test whether the translated/rotated entity
   												// collides with another entity (fast check)
	idBox					testBox;			// to test whether the translated/rotated entity is inside the SEED
												// or collides with another entity (slow, but more precise)
	idBox					box;				// The oriented box of the SEED
	idList<idBounds>		SeedEntityBounds;	// precompute entity bounds for collision checks (fast)
	idList<idBox>			SeedEntityBoxes;	// precompute entity box for collision checks (slow, but thorough)

	idList< int >			ClassIndex;			// random shuffling of classes
	int						s;
	float					f;

	int start = (int) time (NULL);

	idVec3 size = renderEntity.bounds.GetSize();
	// rotating the func-static in DR rotates the brush, but does not change the axis or
	// add a spawnarg, so this will not work properly unless the mapper sets an "angle" spawnarg:
	idMat3 axis = renderEntity.axis;

	gameLocal.Printf( "SEED %s: Origin %0.2f %0.2f %0.2f\n", GetName(), m_origin.x, m_origin.y, m_origin.z );

// DEBUG:	
//	idVec4 markerColor (0.7, 0.2, 0.2, 1.0);
//	idBounds b = idBounds( - size / 2, size / 2 );	
//	gameRenderWorld->DebugBounds( markerColor, b, m_origin, 5000);

	box = idBox( m_origin, size, axis );

	float spacing = spawnArgs.GetFloat( "spacing", "0" );

	idAngles angles = axis.ToAngles();		// debug
	gameLocal.Printf( "SEED %s: Seed %i Size %0.2f %0.2f %0.2f Axis %s.\n", GetName(), m_iSeed_2, size.x, size.y, size.z, angles.ToString());

	m_Entities.Clear();
	if (m_iNumEntities > 100)
	{
		// TODO: still O(N*N) time, tho
		m_Entities.SetGranularity( 64 );	// we append potentially thousands of entries, and every $granularity step
											// the entire list is re-allocated and copied over again, so avoid this
	}
	SeedEntityBounds.Clear();
	SeedEntityBoxes.Clear();

	m_iNumExisting = 0;
	m_iNumVisible = 0;

	// Compute a random order of classes, so that when the mapper specified m_iNumEntities well below
	// the limit (e.g. 1), he gets a random entity from a random class (and not just one from the first
	// class always):

	// remove pseudo classes as we start over fresh
	idList< seed_class_t > newClasses;

	for (int i = 0; i < m_Classes.Num(); i++)
	{
		if (m_Classes[i].pseudo)
		{
			// remove the render model
		   	if (m_Classes[i].hModel)
			{
				renderModelManager->FreeModel( m_Classes[i].hModel );
				m_Classes[i].hModel = NULL;
			}
			// remove the physics object
			if (m_Classes[i].physicsObj)
			{
				delete m_Classes[i].physicsObj;
				m_Classes[i].physicsObj = NULL;
			}
			continue;
		}
		newClasses.Append ( m_Classes[i] );
	}
	m_Classes.Swap( newClasses );		// copy over
	newClasses.Clear();					// remove old entries (including pseudoclasses)

	// Calculate the per-class seed:
	ClassIndex.Clear();			// random shuffling of classes
	for (int i = 0; i < m_Classes.Num(); i++)
	{
		ClassIndex.Append ( i );				// 1,2,3,...
		m_Classes[i].seed = RandomSeed();		// random generator 2 inits the random generator 1
	}

	// Randomly shuffle all entries, but use the second generator for a "predictable" class sequence
	// that does not change when the menu setting changes
	m_iSeed = RandomSeed();
	s = m_Classes.Num();
	for (int i = 0; i < s; i++)
	{
		int second = (int)(RandomFloat() * s);
		int temp = ClassIndex[i]; ClassIndex[i] = ClassIndex[second]; ClassIndex[second] = temp;
	}

	// default random rotate
	idStr rand_rotate_min = spawnArgs.GetString("rotate_min", "0 0 0");
	idStr rand_rotate_max = spawnArgs.GetString("rotate_max", "5 360 5");

	// Compute random positions for all entities that we want to spawn for each class
	for (int idx = 0; idx < m_Classes.Num(); idx++)
	{
		if (m_Entities.Num() >= m_iNumEntities)
		{
			// have enough entities, stop
			break;
		}

		// progress with random shuffled class
		int i = ClassIndex[idx];

		// ignore pseudo classes used for watching brethren only:
		if (m_Classes[i].watch)
		{
			continue;
		}

		m_iSeed = m_Classes[i].seed;		// random generator 2 inits the random generator 1

		// compute the number of entities for this class
		// But try at least one from each class (so "select 1 from 4 classes" works correctly)
		int iEntities = m_Classes[i].maxEntities;
		if (iEntities <= 0)
		{
			iEntities = m_Classes[i].numEntities;
			if (iEntities < 0)
			{
				iEntities = 0;
			}
		}

		gameLocal.Printf( "SEED %s: Creating %i entities of class %s (#%i index %i, seed %i).\n", GetName(), iEntities, m_Classes[i].classname.c_str(), i, idx, m_iSeed );

		// default to what the SEED says
		idAngles class_rotate_min = spawnArgs.GetAngles("seed_rotate_min", rand_rotate_min);
		idAngles class_rotate_max = spawnArgs.GetAngles("seed_rotate_max", rand_rotate_max);

		for (int j = 0; j < iEntities; j++)
		{
			int tries = 0;
			while (tries++ < MAX_TRIES)
			{
				// TODO: allow the "floor" direction be set via spawnarg

				// use bunching? (will always fail if bunching = 0.0)
				// can only use bunching if we have at least one other entity already placed
				if ( m_Entities.Num() > 0 && RandomFloat() < m_Classes[i].bunching )
				{
					// find a random already existing entity of the same class
					// TODO: allow bunching with other classes, too:

					idList <int> BunchEntities;

					// radius
					float distance = m_Classes[i].size.x * m_Classes[i].size.x + 
									 m_Classes[i].size.y * m_Classes[i].size.y; 

					distance = idMath::Sqrt(distance);

					// need minimum the spacing and use maximum 2 times the spacing
					// TODO: make max spacing a spawnarg
					distance += m_Classes[i].spacing * 2; 

					BunchEntities.Clear();
					// build list of all entities we can bunch up to
					for (int e = 0; e < m_Entities.Num(); e++)
					{
						if (m_Entities[e].classIdx == i)
						{
							// same class, try to snuggle up
							BunchEntities.Append(e);
						}
					}
					// select one at random
					int bunchTarget = (float)BunchEntities.Num() * RandomFloat();

					// minimum origin distance (or entity will stick inside the other) is 2 * distance
					// maximum bunch radius is 3 times (2 + 1) the radius
					// TODO: make bunch_size and bunch_min_distance a spawnarg
					SeedEntity.origin = idPolar3( 2 * distance + RandomFloat() * distance / 3, 0, RandomFloat() * 360.0f ).ToVec3();
#ifdef M_DEBUG					
					gameLocal.Printf ("SEED %s: Random origin from distance (%0.2f) %0.2f %0.2f %0.2f (%i)\n",
							GetName(), distance, SeedEntity.origin.x, SeedEntity.origin.y, SeedEntity.origin.z, bunchTarget );
#endif
					// subtract the SEED origin, as m_Entities[ bunchTarget ].origin already contains it and we would
					// otherwise add it twice below:
					SeedEntity.origin += m_Entities[ bunchTarget ].origin - m_origin;
#ifdef M_DEBUG					
					gameLocal.Printf ("SEED %s: Random origin plus bunchTarget origin %0.2f %0.2f %0.2f\n",
							GetName(), SeedEntity.origin.x, SeedEntity.origin.y, SeedEntity.origin.z );
#endif
				}
				else
				// no bunching, just random placement
				{
					// not "none" nor "func"
					if (m_Classes[i].falloff > 0 && m_Classes[i].falloff < 5)
					{
						int falloff_tries = 0;
						float p = 0.0f;
						float factor = m_Classes[i].func_a;
						int falloff = m_Classes[i].falloff;
						if (falloff == 3)
						{
							// X ** 1/N = Nth root of X
							factor = 1 / factor;
						}
						float x = 0;
						float y = 0;
						while (falloff_tries++ < 16)
						{
							// x and y are between -1 and +1
							x = 2.0f * (RandomFloat() - 0.5f);
							y = 2.0f * (RandomFloat() - 0.5f);

							// Then see if it passes the test (inside and higher than the probability)
							// compute distance to center. We skip computing the square root here,
							// because SQRT(X) where X < 1 always produces a result < 1, and if X > 1
							// the result is always > 1, so SQRT() does not change the result in regard
							// to comparing it against 1.0f:
							//float d = idMath::Sqrt( x * x + y * y );
							float d = x * x + y * y;

							if (d > 1.0f)
							{
								// outside the circle, try again
								continue;
							}
							if (falloff == 1)
							{
								// always 1.0f inside the unit-circle for cutoff or func, so abort right away
								p = 1.0f;
								SeedEntity.origin = idVec3( x * size.x / 2, y * size.y / 2, 0 );
								break;
							}

							// compute the probability this position would pass based on "d" (0..1.0f)
							// 4 => linear
							if (falloff == 4)
							{
								p = d;
							}
							// 2 or 3 => pow
							else
							{
								p = idMath::Pow( d, factor );
							}
							// compute a random value and see if it is bigger than p
							if (RandomFloat() > p)
							{
								p = 1.0f;
								break;
							}
							p = 0.0f;
							// nope, not allowed here, try again
						}
						if (p < 0.000001f)
						{
							// did not find a valid position, skip this
							continue;
						}
						//	compute the relative position to our SEED center
						// x/2 => from -1.0 .. 1.0 => -0.5 .. 0.5
						SeedEntity.origin = idVec3( x * size.x / 2, y * size.y / 2, 0 );

					} // end for any falloff other than "none"
					else
					{
						// falloff = none
						// compute a random position in a unit-square
						SeedEntity.origin = idVec3( (RandomFloat() - 0.5f) * size.x, (RandomFloat() - 0.5f) * size.y, 0 );
					}
				}

				// what is the probability it will appear here?
				float probability = 1.0f;	// has passed a potential falloff, so start with "always"

				// if falloff == 5, compute the falloff probability
       			if (m_Classes[i].falloff == 5)
				{
					// p = s * (Xt * x + Yt * y + a)
					float x = (SeedEntity.origin.x / size.x) + 0.5f;		// 0 .. 1.0
					if (m_Classes[i].func_Xt == 2)
					{
						x *= x;							// 2 => X*X
					}

					float y = (SeedEntity.origin.y / size.y) + 0.5f;		// 0 .. 1.0
					if (m_Classes[i].func_Yt == 2)
					{
						y *= y;							// 2 => X*X
					}

					float p = m_Classes[i].func_s * ( x * m_Classes[i].func_x + y * m_Classes[i].func_y + m_Classes[i].func_a);
					// apply custom clamp function
					if (m_Classes[i].func_f == 0)
					{
						if (p < m_Classes[i].func_min || p > m_Classes[i].func_max)
						{
							// outside range, zero-clamp
							//probability = 0.0f;
							// placement will fail, anyway:
							gameLocal.Printf ("SEED %s: Skipping placement, probability == 0 (min %0.2f, p=%0.2f, max %0.2f).\n", 
									GetName(), m_Classes[i].func_min, p, m_Classes[i].func_max );
							continue;
						}
					}
					else
					{
						// clamp to min .. max
						probability = idMath::ClampFloat( m_Classes[i].func_min, m_Classes[i].func_max, p );
					}
					gameLocal.Printf ("SEED %s: falloff func gave p = %0.2f (clamped %0.2f)\n", GetName(), p, probability);
				}

       			// image based falloff probability
				if (m_Classes[i].imgmap)
				{
					// compute the pixel we need to query
					// TODO: add spawnarg-based scaling factor and offset here
					float x = m_Classes[i].map_scale_x * (SeedEntity.origin.x / size.x) + m_Classes[i].map_ofs_x + 0.5f;		// 0 .. 1.0
					float y = m_Classes[i].map_scale_y * (SeedEntity.origin.y / size.y) + m_Classes[i].map_ofs_x + 0.5f;		// 0 .. 1.0

					// if n < 0 or n > 1.0: map back into range 0..1.0
					// second fmod() is for handling negative numbers
					x = fmod( fmod(x, 1.0f) + 1.0, 1.0);
					y = fmod( fmod(y, 1.0f) + 1.0, 1.0);

					// 1 - x to correct for top-left images
					int value = gameLocal.m_ImageMapManager->GetMapDataAt( m_Classes[i].imgmap, 1.0f - x, y );
					if (m_Classes[i].map_invert)
					{
						value = 255 - value;
					}
					//gameLocal.Printf("SEED %s: Pixel at %i, %i (ofs = %i) has value %i (p=%0.2f).\n", GetName(), px, py, ofs, value, (float)value / 256.0f);
					probability *= (float)value / 256.0f;

					if (probability < 0.000001)
					{
						// p too small, continue instead of doing expensive material checks
						continue;
					}
				}

				// Rotate around our rotation axis (to support rotated SEED brushes)
				SeedEntity.origin *= axis;

				// add origin of the SEED
				SeedEntity.origin += m_origin;

				// should only appear on certain ground material(s)?

				// TODO: do the ground trace also: if only appears for certain angles
				// TODO: do the ground trace also: if we rotate the spawned entity to match the ground

				if (m_Classes[i].materials.Num() > 0)
				{
					// end of the trace (downwards the length from entity class position to bottom of SEED)
					idVec3 traceEnd = SeedEntity.origin; traceEnd.z = m_origin.z - size.z;
					// TODO: adjust for different "down" directions
					//vTest *= GetGravityNormal();

					trace_t trTest;
					idVec3 traceStart = SeedEntity.origin;

					//gameLocal.Printf ("SEED %s: TracePoint start %0.2f %0.2f %0.2f end %0.2f %0.2f %0.2f\n",
					//		GetName(), traceStart.x, traceStart.y, traceStart.z, traceEnd.x, traceEnd.y, traceEnd.z );
					gameLocal.clip.TracePoint( trTest, traceStart, traceEnd, 
							CONTENTS_SOLID | CONTENTS_BODY | CONTENTS_CORPSE | CONTENTS_OPAQUE | CONTENTS_MOVEABLECLIP, this );

					// Didn't hit anything?
					if ( trTest.fraction < 1.0f )
					{
						const idMaterial *mat = trTest.c.material;

						surfTypes_t type = mat->GetSurfaceType();
						idStr descr = "";

						// in case the description is empty
						switch (type)
						{
							case SURFTYPE_METAL:
								descr = "metal";
								break;
							case SURFTYPE_STONE:
								descr = "stone";
								break;
							case SURFTYPE_FLESH:
								descr = "flesh";
								break;
							case SURFTYPE_WOOD:
								descr = "wood";
								break;
							case SURFTYPE_CARDBOARD:
								descr = "cardboard";
								break;
							case SURFTYPE_LIQUID:
								descr = "liquid";
								break;
							case SURFTYPE_GLASS:
								descr = "glass";
								break;
							case SURFTYPE_PLASTIC:
								descr = "plastic";
								break;
							case SURFTYPE_15:
								// TODO: only use the first word (until the first space)
								descr = mat->GetDescription();
								break;
							default:
								break;
						}

						// hit something
						//gameLocal.Printf ("SEED %s: Hit something at %0.2f (%0.2f %0.2f %0.2f material %s (%s))\n",
						//	GetName(), trTest.fraction, trTest.endpos.x, trTest.endpos.y, trTest.endpos.z, descr.c_str(), mat->GetName() );

						float p = m_Classes[i].defaultProb;		// the default if nothing hits

						// see if this entity is inhibited by this material
						for (int e = 0; e < m_Classes[i].materials.Num(); e++)
						{
							// starts with the same as the one we look at?
							if ( m_Classes[i].materials[e].name.Find( descr ) == 0 )
							{
								p = m_Classes[i].materials[e].probability;

								//gameLocal.Printf ("SEED %s: Material (%s) matches class material %i (%s), using probability %0.2f\n",
								//		GetName(), descr.c_str(), e, m_Classes[i].materials[e].name.c_str(), probability); 
								// found a match, break
								break;
							}	
						}

						//gameLocal.Printf ("SEED %s: Using probability %0.2f.\n", GetName(), p );

						// multiply probability with p (so 0.5 * 0.5 results in 0.25)
						probability *= p;

						// TODO: angle-of-surface probability

					}	
					else
					{
						// didn't hit anything, floating in air?
					}

				} // end of per-material probability

				// gameLocal.Printf ("SEED %s: Using final p=%0.2f.\n", GetName(), probability );
				// check against the probability (0 => always skip, 1.0 - never skip, 0.5 - skip half)
				float r = RandomFloat();
				if (r > probability)
				{
					//gameLocal.Printf ("SEED %s: Skipping placement, %0.2f > %0.2f.\n", GetName(), r, probability);
					continue;
				}

				if (m_Classes[i].floor)
				{
					//gameLocal.Printf( "SEED %s: Flooring entity #%i.\n", GetName(), j );

					// end of the trace (downwards the length from entity class position to bottom of SEED)
					idVec3 traceEnd = SeedEntity.origin; traceEnd.z = m_origin.z - size.z;
					// TODO: adjust for different "down" directions
					//vTest *= GetGravityNormal();

					// bounds of the class entity
					idVec3 b_1 = - m_Classes[i].size / 2;
					idVec3 b_2 = m_Classes[i].size / 2;
					// assume the entity origin is at the entity bottom
					b_1.z = 0;
					b_2.z = m_Classes[i].size.z;
					idBounds class_bounds = idBounds( b_1, b_2 );
					trace_t trTest;

					idVec3 traceStart = SeedEntity.origin;

					//gameLocal.Printf ("SEED %s: TraceBounds start %0.2f %0.2f %0.2f end %0.2f %0.2f %0.2f bounds %s\n",
					//		GetName(), traceStart.x, traceStart.y, traceStart.z, traceEnd.x, traceEnd.y, traceEnd.z,
					//	   	class_bounds.ToString()	); 
					gameLocal.clip.TraceBounds( trTest, traceStart, traceEnd, class_bounds, 
							CONTENTS_SOLID | CONTENTS_BODY | CONTENTS_CORPSE | CONTENTS_OPAQUE | CONTENTS_MOVEABLECLIP, this );

					// Didn't hit anything?
					if ( trTest.fraction != 1.0f )
					{
						// hit something
						//gameLocal.Printf ("SEED %s: Hit something at %0.2f (%0.2f %0.2f %0.2f)\n",
						//	GetName(), trTest.fraction, trTest.endpos.x, trTest.endpos.y, trTest.endpos.z ); 
						SeedEntity.origin = trTest.endpos;
						SeedEntity.angles = trTest.endAxis.ToAngles();

						// TODO: take trTest.c.normal and angle the entity on this instead

						// TODO: If the model bounds are quite big, but the model itself is "thin"
						// at the bottom (like a tree with a trunk), then the model will "float"
						// in the air. A "min_sink" value can fix this, but only for small inclines.
						// A pine on a 30° slope might still hover 12 units in the air. Let the mapper
						// override the bounds used for collision checks? For instance using a cylinder
						// would already help, using a smaller diameter would help even more.
						// Or could we trace agains the real model?
					}
					else
					{
						// hit nothing
						gameLocal.Printf ("SEED %s: Hit nothing at %0.2f (%0.2f %0.2f %0.2f)\n",
							GetName(), trTest.fraction, SeedEntity.origin.x, SeedEntity.origin.y, SeedEntity.origin.z );
					}
				}
				else
				{
					// just use the Z axis from the editor pos
					SeedEntity.origin.z = m_Classes[i].origin.z;
				}

				// after flooring, check if it is inside z_min/z_max band
       			if ( !m_Classes[i].z_invert )
				{
//						gameLocal.Printf ("SEED %s: z_invert true, min %0.2f max %0.2f cur %0.2f\n", 
  //     						GetName(), m_Classes[i].z_min, m_Classes[i].z_max, SeedEntity.origin.z );
       				if ( SeedEntity.origin.z < m_Classes[i].z_min || SeedEntity.origin.z > m_Classes[i].z_max )
					{
						// outside the band, skip
						continue;
					}
					// TODO: use z_fadein/z_fadeout
				}
				else
				{
	//					gameLocal.Printf ("SEED %s: z_invert false, min %0.2f max %0.2f cur %0.2f\n", 
      // 						GetName(), m_Classes[i].z_min, m_Classes[i].z_max, SeedEntity.origin.z );
					// TODO: use z_fadein/z_fadeout
       				if ( SeedEntity.origin.z > m_Classes[i].z_min && SeedEntity.origin.z < m_Classes[i].z_max )
					{
						// inside the band, skip
						continue;
					}
       				if ( m_Classes[i].z_fadein > 0 && SeedEntity.origin.z < m_Classes[i].z_min + m_Classes[i].z_fadein )
					{
						float d = ((m_Classes[i].z_min + m_Classes[i].z_fadein) - SeedEntity.origin.z) / m_Classes[i].z_fadein;
						probability *= d;
		//				gameLocal.Printf ("SEED %s: d=%02.f new prob %0.2f\n", GetName(), d, probability);
					}
       				if ( m_Classes[i].z_fadeout > 0 && SeedEntity.origin.z > m_Classes[i].z_max - m_Classes[i].z_fadeout )
					{
						float d = (m_Classes[i].z_max - SeedEntity.origin.z) / m_Classes[i].z_fadeout;
						probability *= d;
		//				gameLocal.Printf ("SEED %s: d=%02.f new prob %0.2f\n", GetName(), d, probability);
					}
				}

				if (r > probability)
				{
					//gameLocal.Printf ("SEED %s: Skipping placement, %0.2f > %0.2f.\n", GetName(), r, probability);
					continue;
				}

				// compute a random sink value (that is added ater flooring and after the z-min/max check, so you can
				// have some variability, too)
				if (m_Classes[i].sink_min != 0 || m_Classes[i].sink_max != 0)
				{
					// TODO: use a gravity normal
					float sink = m_Classes[i].sink_min + RandomFloat() * ( m_Classes[i].sink_max - m_Classes[i].sink_min );
					// modify the z-axis according to the sink-value
					SeedEntity.origin.z -= sink;
				}

				// correct for misplaced origins
				SeedEntity.origin += m_Classes[i].offset;
					
				// SeedEntity.origin might now be outside of our oriented box, we check this later

				// randomly rotate
				// pitch, yaw, roll
				SeedEntity.angles = idAngles( 
						class_rotate_min.pitch + RandomFloat() * (class_rotate_max.pitch - class_rotate_min.pitch),
						class_rotate_min.yaw   + RandomFloat() * (class_rotate_max.yaw   - class_rotate_min.yaw  ),
						class_rotate_min.roll  + RandomFloat() * (class_rotate_max.roll  - class_rotate_min.roll ) );
				/*
				gameLocal.Printf ("SEED %s: rand rotate for (%0.2f %0.2f %0.2f) %0.2f %0.2f %0.2f => %s\n", GetName(),
						class_rotate_min.pitch,
						class_rotate_min.yaw,
						class_rotate_min.roll,
						class_rotate_min.pitch + RandomFloat() * (class_rotate_max.pitch - class_rotate_min.pitch),
						class_rotate_min.yaw   + RandomFloat() * (class_rotate_max.yaw   - class_rotate_min.yaw  ),
						class_rotate_min.roll  + RandomFloat() * (class_rotate_max.roll  - class_rotate_min.roll ), SeedEntity.angles.ToString() );
				*/

				// inside SEED bounds?
				// IntersectsBox() also includes touching, but we want the entity to be completely inside
				// so we just check that the origin is inside, which is also faster:
				// The entity can stick still outside, we need to "shrink" the testbox by half the class size
				if (box.ContainsPoint( SeedEntity.origin ))
				{
					//gameLocal.Printf( "SEED %s: Entity would be inside our box. Checking against inhibitors.\n", GetName() );

					testBox = idBox ( SeedEntity.origin, m_Classes[i].size, SeedEntity.angles.ToMat3() );

					// only if this class can be inhibited
					if (! m_Classes[i].noinhibit)
					{
						bool inhibited = false;
						for (int k = 0; k < m_Inhibitors.Num(); k++)
						{
							// TODO: do a faster bounds check first?
							// this test ensures that entities "peeking" into the inhibitor will be inhibited, too
							if (testBox.IntersectsBox( m_Inhibitors[k].box ) )
							{
								// inside an inhibitor
								inhibited = true;		// default is inhibit
								
								// check against classnames and allow/inhibit
								int n = m_Inhibitors[k].classnames.Num();
								if (n > 0)
								{
									// "inhibit" set => inhibit_only true => start with false
									// "noinhibit" set and "inhibit" not set => inhibit_only false => start with true
									inhibited = ! m_Inhibitors[k].inhibit_only;
									for (int c = 0; c < n; c++)
									{
										if (m_Inhibitors[k].classnames[c] == m_Classes[i].classname)
										{
											// flip the true/false value if we found a match
											inhibited = !inhibited;
											gameLocal.Printf( "SEED %s: Entity class %s %s by inhibitor %i.\n", 
													GetName(), m_Classes[i].classname.c_str(), inhibited ? "inhibited" : "allowed", k );
											break;
										}
									}
								}

								if (inhibited == true && m_Inhibitors[k].falloff > 0)
								{
									// if it would have been inhibited in the first place, see if the
									// falloff does allow it, tho:
									float p = 1.0f;						// probability that it gets inhibitied

									float factor = m_Inhibitors[k].factor;
									int falloff = m_Inhibitors[k].falloff;
									if (falloff == 3)
									{
										// X ** 1/N = Nth root of X
										factor = 1 / factor;
									}
									// distance to inhibitor center, normalized to 1x1 square
									float x = 2.0f * (SeedEntity.origin.x - m_Inhibitors[k].origin.x) / m_Inhibitors[k].size.x;
									float y = 2.0f * (SeedEntity.origin.y - m_Inhibitors[k].origin.y) / m_Inhibitors[k].size.y;
									// Skip computing the SQRT() since sqrt(1) == 1, sqrt(d < 1) < 1 and sqrt(d > 1) > 1:
									float d = x * x + y * y;
									// outside, gets not inhibited
									inhibited = false;
									// inside the circle?
									if (d < 1.0f)
									{
										if (falloff == 1)
										{
											// cutoff - always inhibit
											p = 0.0f;
										}
										else
										{
											if (falloff == 4)
											{
												// 4 - linear
												p = d;
											}
											else
											{
												// 2 or 3
												p = idMath::Pow(d, factor);
											}
										}
										// 5 - func (not implemented yet)
										// if a random number is greater than "p", it will get prohibitied
										if (RandomFloat() > p)
										{
											//gameLocal.Printf( "SEED %s: Entity inhibited by inhibitor %i. Trying new place.\n", GetName(), k );
											inhibited = true;
											break;
										}
									}
								}
							}
						}

						if ( inhibited )
						{
							continue;
						}
					}

					// check the min. spacing constraint
				 	float use_spacing = spacing;
					if (m_Classes[i].spacing != 0)
					{
						use_spacing = m_Classes[i].spacing;
					}

					// gameLocal.Printf( "SEED %s: Using spacing constraint %0.2f for entity %i.\n", GetName(), use_spacing, j );

					// check that the entity does not collide with any other entity
					if (m_Classes[i].nocollide > 0 || use_spacing > 0)
					{
						bool collides = false;

						// expand the testBounds and testBox with the spacing
						testBounds = (idBounds( m_Classes[i].size ) + SeedEntity.origin) * SeedEntity.angles.ToMat3();
						testBounds.ExpandSelf( use_spacing );
						testBox.ExpandSelf( use_spacing );

						for (int k = 0; k < m_Entities.Num(); k++)
						{
							// do a quick check on bounds first
							idBounds otherBounds = SeedEntityBounds[k];
							if (otherBounds.IntersectsBounds (testBounds))
							{
								//gameLocal.Printf( "SEED %s: Entity %i bounds collides with entity %i bounds, checking box.\n", GetName(), j, k );
								// do a thorough check against the box here

								idBox otherBox = SeedEntityBoxes[k];
								if (otherBox.IntersectsBox (testBox))
								{
									gameLocal.Printf( "SEED %s: Entity %i box collides with entity %i box, trying another place.\n", GetName(), j, k );
									collides = true;
									break;
								}
								// no collision, place is usable
							}
						}
						if (collides)
						{
							continue;
						}
					}

					if (tries < MAX_TRIES && m_iDebug > 0)
					{
						gameLocal.Printf( "SEED %s: Found valid position for entity %i with %i tries.\n", GetName(), j, tries );
					}
					break;
				}
				else
				{
					// gameLocal.Printf( "SEED %s: Test position outside our box, trying again.\n", GetName() );
				}
			}
			// couldn't place entity even after 10 tries?
			if (tries >= MAX_TRIES) continue;

			// compute a random color value
			idVec3 color = m_Classes[i].color_max - m_Classes[i].color_min; 
			color.x = color.x * RandomFloat() + m_Classes[i].color_min.x;
			color.y = color.y * RandomFloat() + m_Classes[i].color_min.y;
			color.z = color.z * RandomFloat() + m_Classes[i].color_min.z;
			// and store it packed
			SeedEntity.color = PackColor( color );

			// choose skin randomly
			SeedEntity.skinIdx = m_Classes[i].skins[ RandomFloat() * m_Classes[i].skins.Num() ];
			//gameLocal.Printf( "SEED %s: Using skin %i.\n", GetName(), SeedEntity.skinIdx );
			// will be automatically spawned when we are in range
			SeedEntity.flags = SEED_ENTITY_HIDDEN; // but not SEED_ENTITY_EXISTS nor SEED_ENTITY_FIRSTSPAWN

			// TODO: add waiting flag and enter in waiting_queue if wanted
			SeedEntity.entity = 0;
			SeedEntity.classIdx = i;

			// compute a random value between scale_min and scale_max
			if (m_Classes[i].scale_min.x == 0)
			{
				// axes-equal scaling
				float factor = RandomFloat() * (m_Classes[i].scale_max.z - m_Classes[i].scale_min.z) + m_Classes[i].scale_min.z;
				SeedEntity.scale = idVec3( factor, factor, factor );
			}
			else
			{
				idVec3 scale = m_Classes[i].scale_max - m_Classes[i].scale_min; 
				scale.x = scale.x * RandomFloat() + m_Classes[i].scale_min.x;
				scale.y = scale.y * RandomFloat() + m_Classes[i].scale_min.y;
				scale.z = scale.z * RandomFloat() + m_Classes[i].scale_min.z;
				SeedEntity.scale = scale;
			}

			// precompute bounds for a fast collision check
			SeedEntityBounds.Append( (idBounds (m_Classes[i].size ) + SeedEntity.origin) * SeedEntity.angles.ToMat3() );
			// precompute box for slow collision check
			SeedEntityBoxes.Append( idBox ( SeedEntity.origin, m_Classes[i].size, SeedEntity.angles.ToMat3() ) );
			m_Entities.Append( SeedEntity );

			if (m_Entities.Num() >= m_iNumEntities)
			{
				// have enough entities, stop
				break;
			}
		}
	}

	// if we have requests for watch brethren, do add them now
	for (int i = 0; i < m_Classes.Num(); i++)
	{
		// only care for classes where we watch an entity
		if (!m_Classes[i].watch)
		{
			continue;
		}
		// go through all entities
		for (int j = 0; j < gameLocal.num_entities; j++)
		{
			idEntity *ent = gameLocal.entities[ j ];

			if (!ent)
			{
				continue;
			}
			idVec3 origin = ent->GetPhysics()->GetOrigin();

			// the class we should watch?
			// also compare the "model" spawnarg, otherwise multiple func_statics won't work
			if (( ent->GetEntityDefName() == m_Classes[i].classname) &&
				// and is this entity in our box?
				(box.ContainsPoint( origin )) )
			{
				gameLocal.Printf( "SEED %s: Watching over brethren %s at %02f %0.2f %0.2f.\n", GetName(), ent->GetName(), origin.x, origin.y, origin.z );
				// add this entity to our list
				SeedEntity.origin = origin;
				SeedEntity.angles = ent->GetPhysics()->GetAxis().ToAngles();
				// support "random_skin" by looking at the actual set skin:
				idStr skin = ent->GetSkin()->GetName();
				SeedEntity.skinIdx = AddSkin( &skin );
				// already exists, already visible and spawned
				SeedEntity.flags = SEED_ENTITY_EXISTS + SEED_ENTITY_SPAWNED;
				SeedEntity.entity = j;
				SeedEntity.classIdx = i;
				m_Entities.Append( SeedEntity );
			}
		}
	}

	int end = (int) time (NULL);
	gameLocal.Printf("SEED %s: Preparing %i entities took %i seconds.\n", GetName(), m_Entities.Num(), end - start );

	// combine the spawned entities into megamodels if possible
	CombineEntities();
}

// sort a list of offsets by their distance
int SortOffsetsByDistance( const seed_sort_ofs_t *a, const seed_sort_ofs_t *b ) {
	float d = a->ofs.offset.LengthSqr() - b->ofs.offset.LengthSqr();

	if ( d < 0 ) {
		return -1;
	}
	if ( d > 0 ) {
		return 1;
	}
	return 0;
}

// compute the LOD distance for this delta vector and for this entity
float Seed::LODDistance( const lod_data_t* m_LOD, idVec3 delta ) const
{
	if( m_LOD && m_LOD->bDistCheckXYOnly )
	{
		// TODO: do this per-entity
		idVec3 vGravNorm = GetPhysics()->GetGravityNormal();
		delta -= (vGravNorm * delta) * vGravNorm;
	}

	// multiply with the user LOD bias setting, and return the result:
	float bias = cv_lod_bias.GetFloat();
	return delta.LengthSqr() / (bias * bias);
}

// a small helper routine to cut down on code copy&paste
bool Seed::SetClipModelForMulti( idPhysics_StaticMulti* physics, const idStr modelName, const seed_entity_t* entity, const int idx, idClipModel* clipModel )
{
	idClipModel *clip;

	// load the clipmodel for the lowest LOD stage for collision detection
	bool clipLoaded = true;

	if (clipModel)
	{
		// make a copy
		clip = new idClipModel( clipModel );
#ifdef M_DEBUG
		gameLocal.Printf("Reusing clipmodel from renderModel 0x%p, bounds %s.\n", clipModel, clip->GetBounds().ToString() );
#endif
	}
	else
	{
#ifdef M_DEBUG
		gameLocal.Printf("Loading clip for %s.\n", modelName.c_str());
#endif
		clip = new idClipModel;
		clipLoaded = clip->LoadModel( modelName );
	}

	if (clipLoaded)
	{
		// add the clipmodel
		physics->SetClipModel(clip, 1.0f, idx, true);

		physics->SetOrigin( entity->origin, idx);
		physics->SetAxis( entity->angles.ToMat3(), idx);
		// Scale the clipmodel
		physics->Scale( entity->scale );
		// Make it solid
		physics->SetContents( MASK_SOLID | CONTENTS_MOVEABLECLIP | CONTENTS_RENDERMODEL, idx );
		// nec.?
		physics->SetClipMask( MASK_SOLID | CONTENTS_MOVEABLECLIP | CONTENTS_RENDERMODEL);
	}
	return clipLoaded;
}

void Seed::CombineEntities( void )
{
	bool multiPVS = m_iNumPVSAreas > 1 ? true : false;
	idList < int > pvs;								//!< in which PVS is this entity?
	idBounds modelAbsBounds;						//!< for per-entity PVS check
	idBounds entityBounds;							//!< for per-entity PVS check
	int iNumPVSAreas = 0;							//!< for per-entity PVS check
	int iPVSAreas[2];								//!< for per-entity PVS check
	seed_class_t PseudoClass;
	idList< seed_entity_t > newEntities;
	unsigned int mergedCount = 0;
	idList < seed_sort_ofs_t > sortedOffsets;		//!< To merge the N nearest entities
	model_ofs_t ofs;
	seed_sort_ofs_t sortOfs;

	if ( !m_bCombine )
	{
		gameLocal.Printf("SEED %s: combine = 0, skipping combine step.\n", GetName() );
		return;
	}

	float max_combine_distance = spawnArgs.GetFloat("combine_distance", "1024");
	if (max_combine_distance < 10)
	{
		gameLocal.Warning("SEED %s: combine distance %0.2f < 10, enforcing minimum 10.\n", GetName(), max_combine_distance);
		max_combine_distance = 10;
	}
	// square for easier comparing
	max_combine_distance *= max_combine_distance;

	int start = (int) time (NULL);

	// Get the player pos
	idPlayer *player = gameLocal.GetLocalPlayer();
	// if we have no player (how can this happen?), use our own origin as stand-in
	idVec3 playerPos = renderEntity.origin;
	if ( player )
	{
		playerPos = player->GetPhysics()->GetOrigin();
	}

	// for each entity, find out in which PVS it is, unless we only have one PVS on the seed,
	// we then expect all entities to be in the same PVS, too:
	if (multiPVS)
	{
		gameLocal.Printf("SEED %s: MultiPVS.\n", GetName() );
		pvs.Clear();
		// O(N)
		for (int i = 0; i < m_Entities.Num(); i++)
		{
			// find out in which PVS this entity is
			idVec3 size = m_Classes[ m_Entities[i].classIdx ].size / 2; 
		    modelAbsBounds.FromTransformedBounds( idBounds( -size, size ), m_Entities[i].origin, m_Entities[i].angles.ToMat3() );
			int iNumPVSAreas = gameLocal.pvs.GetPVSAreas( modelAbsBounds, iPVSAreas, sizeof( iPVSAreas ) / sizeof( iPVSAreas[0] ) );
			if (iNumPVSAreas > 1)
			{
				// more than one PVS area, never combine this entity
				pvs.Append(-1);
			}
			else
			{
				// remember this value
				pvs.Append( iPVSAreas[0] );
			}
		}
	}
	else
	{
		gameLocal.Printf("SEED %s: SinglePVS.\n", GetName() );
	}

	idRenderModel* tempModel = NULL;

	int n = m_Entities.Num();
	// we mark all entities that we combine with another entity with "-1" in the classIdx
	for (int i = 0; i < n - 1; i++)
	{
		unsigned int merged = 0;				//!< merged 0 other entities into this one

		//gameLocal.Printf("SEED %s: At entity %i\n", GetName(), i);
		if (m_Entities[i].classIdx < 0)
		{
			// already combined, skip
			//gameLocal.Printf("SEED %s: Entity %i already combined into another entity, skipping it.\n", GetName(), i);
			continue;
		}

		const seed_class_t * entityClass = & m_Classes[ m_Entities[i].classIdx ];

		// if this class says no combine, skip it
		if (entityClass->nocombine)
		{
			continue;
		}

		tempModel = entityClass->hModel;
		if (NULL == tempModel)
		{
			// load model, then combine away
//			gameLocal.Warning("SEED %s: Load model %s for entity %i.\n", GetName(), entityClass->modelname.c_str(), i);
			tempModel = renderModelManager->FindModel( entityClass->modelname );
			if (! tempModel)
			{
				gameLocal.Warning("SEED %s: Could not load model %s for entity %i, skipping it.\n", GetName(), entityClass->modelname.c_str(), i);
				continue;
			}
		}

		sortedOffsets.Clear();
		sortedOffsets.SetGranularity(64);	// we might have a few hundred entities in there

		ofs.offset = idVec3(0,0,0); // the first copy is the original
		ofs.angles = m_Entities[i].angles;

		// compute the alpha value and the LOD level
		float fAlpha  = ThinkAboutLOD( entityClass->m_LOD, LODDistance( entityClass->m_LOD, m_Entities[i].origin - playerPos ) );
		// 0 => default model, 1 => first stage etc
		ofs.lod	   = m_LODLevel + 1;
//		gameLocal.Warning("SEED %s: Using LOD model %i for base entity.\n", GetName(), ofs.lod );
		// TODO: pack in the correct alpha value
		ofs.color  = m_Entities[i].color;
		ofs.scale  = m_Entities[i].scale;
		ofs.flags  = 0;

		// restore our value (it is not used, anyway)
		m_LODLevel = 0;

		sortOfs.ofs = ofs; sortOfs.entity = i; sortedOffsets.Append (sortOfs);

		// how many can we combine at most?
		// use LOD 0 here for an worse-case estimate
		unsigned int maxModelCount = gameLocal.m_ModelGenerator->GetMaxModelCount( tempModel );
		gameLocal.Printf("SEED %s: Combining at most %u models for entity %i.\n", GetName(), maxModelCount, i );

		// try to combine as much entities into this one
		// O(N*N) performance, but only if we don't combine any entities, otherwise
		// every combine step reduces the number of entities to look at next:
		for (int j = i + 1; j < n; j++)
		{
			//gameLocal.Printf("SEED %s: %i: At entity %i\n", GetName(), i, j);
			if (m_Entities[j].classIdx == -1)
			{
				// already combined, skip
				//gameLocal.Printf("SEED %s: Entity %i already combined into another entity, skipping it.\n", GetName(), j);
				continue;
			}
			if (m_Entities[j].classIdx != m_Entities[i].classIdx)
			{
				// have different classes
//				gameLocal.Printf("SEED %s: Entity classes from %i (%i) and %i (%i) differ, skipping it.\n", GetName(), i, m_Entities[i].classIdx, j, m_Entities[j].classIdx);
				continue;
			}
			if (m_Entities[j].skinIdx != m_Entities[i].skinIdx)
			{
				// have different skins
//				gameLocal.Printf("SEED %s: Entity skins from %i and %i differ, skipping it.\n", GetName(), i, j);
				continue;
			}
			// in different PVS?
			if ( multiPVS && pvs[j] != pvs[i])
			{
//				gameLocal.Printf("SEED %s: Entity %i in different PVS than entity %i, skipping it.\n", GetName(), j, i);
				continue;
			}
			// distance too big?
			idVec3 dist = (m_Entities[j].origin - m_Entities[i].origin);
			float distSq = dist.LengthSqr();
			if (distSq > max_combine_distance)
			{
				// gameLocal.Printf("SEED %s: Distance from entity %i to entity %i to far (%f > %f), skipping it.\n", GetName(), j, i, dist.Length(), max_combine_distance );
				continue;
			}

			ofs.offset = dist;
			ofs.angles = m_Entities[j].angles;

			// compute the alpha value and the LOD level
			float fAlpha = ThinkAboutLOD( entityClass->m_LOD, LODDistance( entityClass->m_LOD, m_Entities[i].origin - playerPos ) );
			// 0 => default model, 1 => level 0 etc.
			ofs.lod		= m_LODLevel + 1;
//			gameLocal.Warning("SEED %s: Using LOD model %i for combined entity %i.\n", GetName(), ofs.lod, j );
			// TODO: pack in the new alpha value
			ofs.color  = m_Entities[j].color;
			ofs.scale  = m_Entities[j].scale;
			ofs.flags  = 0;
			// restore our value (it is not used, anyway)
			m_LODLevel = 0;

			sortOfs.ofs = ofs; sortOfs.entity = j; sortedOffsets.Append (sortOfs);

			if (merged == 0)
			{
				PseudoClass.pseudo = true;
				PseudoClass.m_LOD = entityClass->m_LOD;
				PseudoClass.modelname = entityClass->modelname;
				PseudoClass.spawnDist = entityClass->spawnDist;
				PseudoClass.cullDist = entityClass->cullDist;
				PseudoClass.size = entityClass->size;
				PseudoClass.solid = entityClass->solid;
				PseudoClass.clip = entityClass->clip;
				PseudoClass.imgmap = 0;
				PseudoClass.score = 0;
				PseudoClass.offset = entityClass->offset;
				PseudoClass.numEntities = 0;
				PseudoClass.maxEntities = 0;
				// a combined entity must be of this class to get the multi-clipmodel working
				PseudoClass.classname = FUNC_DUMMY;
				// in case the combined model needs to be combined from multiple func_statics
				PseudoClass.hModel = entityClass->hModel;
				PseudoClass.physicsObj = new idPhysics_StaticMulti;

				PseudoClass.physicsObj->SetContents( CONTENTS_RENDERMODEL );
			}
			// for this entity
			merged ++;
			// overall
			mergedCount ++;
//			gameLocal.Printf("SEED %s: Merging entity %i (origin %s) into entity %i (origin %s, dist %s).\n", 
//					GetName(), j, m_Entities[j].origin.ToString(), i, m_Entities[i].origin.ToString(), dist.ToString() );

			// mark with negative classIdx so we can skip it, or restore the classIdx (be negating it again)
			m_Entities[j].classIdx = -m_Entities[j].classIdx;
		}

		if (merged > 0)
		{

			idStr lowest_LOD_model = entityClass->modelname;	// default for no LOD

			// if entities of this class have LOD:
			if (entityClass->m_LOD)
			{
				lod_data_t* tmlod = entityClass->m_LOD;	// shortcut
				
				// try to load all LOD models in LODs to see if they exist
				for (int mi = 0; mi < LOD_LEVELS; mi++)
				{
					// load model, then combine away
//					gameLocal.Warning("SEED %s: Trying to load LOD model #%i %s for entity %i.", 
//							GetName(), mi, tmlod->ModelLOD[mi].c_str(), i);

					idStr* mName = &(tmlod->ModelLOD[mi]);
					if (! mName->IsEmpty() )
					{
						idRenderModel* tModel = renderModelManager->FindModel( mName->c_str() );
						if (!tModel)
						{
							gameLocal.Warning("SEED %s: Could not load LOD model #%i %s for entity %i, skipping it.", 
									GetName(), mi, mName->c_str(), i);
						}
						else
						{
							lowest_LOD_model = mName->c_str();
						}
					}
				}
			}

			// if we have more entities to merge than what will fit into the model,
			// sort them based on distance and select the N nearest:
			if (merged > maxModelCount)
			{
				// sort the offsets so we can select the N nearest
				sortedOffsets.Sort( SortOffsetsByDistance );

				// for every entity after the first "maxModelCount", restore their class index
				// so they get later checked again
				for (int si = maxModelCount; si < sortedOffsets.Num(); si++)
				{
					int idx = sortedOffsets[si].entity;
					m_Entities[ idx ].classIdx = -m_Entities[idx].classIdx;
				}
				// now truncate to only combine as much as we can:
				gameLocal.Printf( " merged %i > maxModelCount %i\n", merged, maxModelCount);
				sortedOffsets.SetNum( maxModelCount );
			}
			// build the offsets list
			PseudoClass.offsets.Clear();
			PseudoClass.offsets.SetGranularity(64);
			for (int si = 0; si < sortedOffsets.Num(); si++)
			{
				PseudoClass.offsets.Append( sortedOffsets[si].ofs );
			}

			bool clipLoaded = false;
			// if the original entity has "solid" "0", skip the entire clip model loading/setting:
			if (entityClass->solid)
			{
				// Load or use the clipmodel
				clipLoaded = SetClipModelForMulti( PseudoClass.physicsObj, lowest_LOD_model, &m_Entities[i], 0, PseudoClass.clip );
				if (!clipLoaded)
				{
					gameLocal.Warning("SEED %s: Could not load clipmodel for %s.\n", GetName(), lowest_LOD_model.c_str() );
				}
			}

			if (clipLoaded)
			{
//				gameLocal.Printf("SEED %s: Loaded clipmodel (bounds %s) for %s.\n",
//						GetName(), lod_0_clip->GetBounds().ToString(), lowest_LOD_model.c_str() );

				// TODO: expose this so we avoid resizing the clipmodel idList for every model we add:
				// PseudoClass.physicsObj->SetClipModelsNum( merged > maxModelCount ? maxModelCount : merged );
				PseudoClass.physicsObj->SetOrigin( m_Entities[i].origin);		// need this
				PseudoClass.physicsObj->SetAxis( idAngles(0,0,0).ToMat3() );	// need to set zero rotation
			}

			// mark all entities that will be merged as "deleted", but skip the rest
			unsigned int n = (unsigned int)sortedOffsets.Num();
			for (unsigned int d = 0; d < n; d++)
			{
				int todo = sortedOffsets[d].entity;
				// mark as combined
				m_Entities[todo].classIdx = -1;

				// add the clipmodel to the multi-clipmodel if we have one
				if (clipLoaded)
				{
					// d + 1 because 0 is the original entity
					SetClipModelForMulti( PseudoClass.physicsObj, lowest_LOD_model, &m_Entities[todo], d + 1, PseudoClass.clip );

//					gameLocal.Printf("Set clipmodel bounds %s\n", PseudoClass.physicsObj->GetClipModel( d + 1 )->GetBounds().ToString() );
				}
			}
			gameLocal.Printf("SEED %s: Combined %i entities, used %s clipmodel.\n", GetName(), sortedOffsets.Num(), clipLoaded ? "a" : "no" );
			sortedOffsets.Clear();

			// build the combined model
			PseudoClass.materialName = "";
			if (m_bDebugColors)
			{
				// select one at random
				PseudoClass.materialName = idStr("textures/darkmod/debug/") + seed_debug_materials[ gameLocal.random.RandomInt( SEED_DEBUG_MATERIAL_COUNT ) ];
			}

			// replace the old class with the new pseudo class
			m_Entities[i].classIdx = m_Classes.Append( PseudoClass );

			// marks as using a pseudo class
			m_Entities[i].flags += SEED_ENTITY_PSEUDO;

			// don't try to rotate the combined model after spawn
			m_Entities[i].angles = idAngles(0,0,0);
		}
	}

	if (mergedCount > 0)
	{
		gameLocal.Printf("SEED %s: Merged entity positions, now building combined final list.\n", GetName() );

		// delete all entities that got merged

		newEntities.Clear();
		// avoid low performance when we append one-by-one with occasional copy of the entire list
		// TODO: still O(N*N) time, tho
		if (m_Entities.Num() - mergedCount > 100)
		{
			newEntities.SetGranularity(64);
		}
		for (int i = 0; i < m_Entities.Num(); i++)
		{
			// we marked all entities that we combine with another entity with "-1" in the classIdx, so skip these
			if (m_Entities[i].classIdx != -1)
			{
				newEntities.Append( m_Entities[i] );
			}
		}
		m_Entities.Swap( newEntities );
		newEntities.Clear();				// should get freed at return, anyway

	}

	// TODO: is this nec.?
	sortedOffsets.Clear();

	int end = (int) time (NULL);
	gameLocal.Printf("SEED %s: Combined %i entities into %i entities, took %i seconds.\n", GetName(), mergedCount + m_Entities.Num(), m_Entities.Num(), end - start );

	return;
}

/*
================
Seed::SpawnEntity - spawn the entity with the given index, returns true if it was spawned
================
*/

bool Seed::SpawnEntity( const int idx, const bool managed )
{
	struct seed_entity_t* ent = &m_Entities[idx];
	struct seed_class_t*  lclass = &(m_Classes[ ent->classIdx ]);

	// spawn the entity and note its number
	if (m_iDebug)
	{
		gameLocal.Printf( "SEED %s: Spawning entity #%i (%s, skin %s, model %s), managed: %s.\n",
				GetName(), idx, lclass->classname.c_str(), m_Skins[ ent->skinIdx ].c_str(), 
				lclass->modelname.c_str(),
				managed ? "yes" : "no" );
	}

	// avoid that we run out of entities during run time
	if (gameLocal.num_entities > SPAWN_LIMIT)
	{
		return false;
	}

	// TODO: Limit number of entities to spawn per frame

	const char* pstr_DefName = lclass->classname.c_str();

	const idDict *p_Def = gameLocal.FindEntityDefDict( pstr_DefName, false );
	if( p_Def )
	{
		idEntity *ent2;
		idDict args;

		args.Set("classname", lclass->classname);

		// has a model?
		args.Set("model", lclass->modelname);

		// move to right place
		args.SetVector("origin", ent->origin );

		// set previously defined (possible random) skin
	    args.Set("skin", m_Skins[ ent->skinIdx ] );
		// disable any other random_skin on the entity class or it would interfere
	    args.Set("random_skin", "");

		// set previously defined (possible random) color
		idVec3 clr; UnpackColor( ent->color, clr );
	    args.SetVector("_color", clr );

		// set floor to 0 to avoid moveables to be floored
	    args.Set("floor", "0");

		// TODO: spawn as hidden, then later unhide them via LOD code
		//args.Set("hide", "1");
		// disable LOD checks on entities (we take care of this)
		if (managed)
		{
			args.Set("dist_check_period", "0");
		}

		gameLocal.SpawnEntityDef( args, &ent2 );
		if (ent2)
		{
			// TODO: check if the entity has been spawned for the first time and if so,
			// 		 also take control of any attachments it has? Or spawn it during build
			//		 and then parse the attachments as new class?

			//gameLocal.Printf( "SEED %s: Spawned entity #%i (%s) at  %0.2f, %0.2f, %0.2f.\n",
			//		GetName(), i, lclass->classname.c_str(), ent->origin.x, ent->origin.y, ent->origin.z );

			ent->entity = ent2->entityNumber;
			// and rotate
			// TODO: Would it be faster to set this as spawnarg before spawn?
			ent2->SetAxis( ent->angles.ToMat3() );
			if (managed)
			{
				ent2->BecomeInactive( TH_THINK );
			}
			// Tell the entity to disable LOD on all it's attachments, and handle
			// them ourselves.
			//idStaticEntity *s_ent = static_cast<idStaticEntity*>( ent2 );
			/* TODO: Disable LOD for attachments, too, then manage them outselves.
			   Currently, if you spawn 100 entities with 1 attachement each, we
			   save thinking on 100 entities, but still have 100 attachements think.
			if (s_ent)
			{
				s_ent->StopLOD( true );
			}
			*/
			m_iNumExisting ++;
			m_iNumVisible ++;

			// Is this an idStaticEntity? If yes, simply spawning will not recreate the model
			// so we need to do this manually.
			if ( lclass->pseudo || lclass->classname == FUNC_DUMMY )
			{
				// cache this
				renderEntity_t *r = ent2->GetRenderEntity();
				
				if (lclass->pseudo || lclass->hModel)
				{
					ent2->FreeModelDef();
					// keep the actual model around, because someone else might have a ptr to it:
					//renderModelManager->FreeModel( r->hModel );
					r->hModel = NULL;
				}

				// gameLocal.Printf("%s: %i physics=%p model=%p\n", GetName(), lclass->pseudo, lclass->physicsObj, lclass->hModel);
				// setup the rendermodel and the clipmodel
				if (lclass->pseudo)
				{
					// each pseudoclass spawns only one entity
					// gameLocal.Printf ("Enabling pseudoclass model %s\n", lclass->classname.c_str() );

					ent2->SetPhysics( lclass->physicsObj );

					lclass->physicsObj->SetSelf( ent2 );
					// TODO: lclass->origin?
					lclass->physicsObj->SetOrigin( ent->origin );

					// tell the CStaticMulti entity that it should track updates:
					CStaticMulti *sment = static_cast<CStaticMulti*>( ent2 );

					// Let the StaticMulti store the nec. data to create the combined rendermodel
					sment->SetLODData( lclass->m_LOD, lclass->modelname, &lclass->offsets, lclass->materialName, lclass->hModel );
					
					// Register the new staticmulti entity with ourselves, so we can later Restore() it properly
					m_iNumStaticMulties ++;

					// enable thinking (mainly for debug draw)
					ent2->BecomeActive( TH_THINK | TH_PHYSICS );
				}
				else
				{
					// a "not-combined" entity
					if (lclass->hModel)
					{
						// just duplicate it (for func_statics from map geometry), with a possible rescaling
						r->hModel = gameLocal.m_ModelGenerator->DuplicateModel( lclass->hModel, lclass->classname, true, NULL, &ent->scale );
						if ( r->hModel )
						{
							// take the model bounds and transform them for the renderentity
							r->bounds.FromTransformedBounds( lclass->hModel->Bounds(), r->origin, r->axis );
						}
						else
						{
							// should not happen
							r->bounds.Zero();
						}
						// force an update because the bounds/origin/axis may stay the same while the model changes
						r->forceUpdate = true;

						// set the correct clipmodel (to override the "plank" one)
						if (lclass->clip)
						{
							idClipModel *clip = new idClipModel( lclass->clip );	// make a copy
							idPhysics *p = ent2->GetPhysics();
							// translate the copy to the correct position
							clip->Translate( p->GetOrigin() - clip->GetOrigin() );
							p->SetClipModel( clip, 1.0f, 0, true );		// true => free old clipmodel
						}

						// nec. to make the entity appear visually
						ent2->Present();
					}
					// else: the correct model was already loaded
				}

				// short version of "UpdateVisuals()"
				// set to invalid number to force an update the next time the PVS areas are retrieved
				ent2->ClearPVSAreas();
			}
			else
			{
				// might be a moveable?
				if ( ent2->IsType( idMoveable::Type ) ) {
					idMoveable *ment = static_cast<idMoveable*>( ent2 );
					ment->ActivatePhysics( this );

					// first spawn ever?
					if ((ent->flags & SEED_ENTITY_SPAWNED) == 0)
					{
				   		// add a random impulse
						// spherical coordinates: radius (magnitude), theta (inclination +-90°), phi (azimut 0..369°)
						idVec3 impulse = lclass->impulse_max - lclass->impulse_min; 
						impulse.x = impulse.x * RandomFloat() + lclass->impulse_min.x;
						impulse.y = impulse.y * RandomFloat() + lclass->impulse_min.y;
						impulse.z = impulse.z * RandomFloat() + lclass->impulse_min.z;
						// gameLocal.Printf("SEED %s: Applying random impulse (polar %s, vec3 %s) to entity.\n", GetName(), impulse.ToString(), idPolar3( impulse ).ToVec3().ToString() );
						ent2->GetPhysics()->SetLinearVelocity( 
							// ent2->GetPhysics()->GetLinearVelocity() +
							idPolar3( impulse ).ToVec3() );
						//ent2->ApplyImpulse( this, 0, ent->origin, idPolar3( impulse ).ToVec3() );
					}

				}
			}

			// preserve PSEUDO flag
			ent->flags = SEED_ENTITY_SPAWNED + SEED_ENTITY_EXISTS + (ent->flags & SEED_ENTITY_PSEUDO);

			return true;
		}
	}
	return false;
}

/*
================
Seed::CullEntity - cull the entity with the given index, returns true if it was culled
================
*/
bool Seed::CullEntity( const int idx )
{
	struct seed_entity_t* ent = &m_Entities[idx];
	struct seed_class_t*  lclass = &(m_Classes[ ent->classIdx ]);

	if ( (ent->flags & SEED_ENTITY_EXISTS) == 0 )
	{
		return false;
	}

	// cull (remove) the entity
	idEntity *ent2 = gameLocal.entities[ ent->entity ];
	if (ent2)
		{
		// Before we remove the entity, save it's position and angles
		// That makes it work for moveables or anything else that
		// might have changed position (teleported away etc)
		// TODO: this might be responsible for CStaticMulti shifting away

		ent->origin = ent2->GetPhysics()->GetOrigin();
		ent->angles = ent2->GetPhysics()->GetAxis().ToAngles();

		// gameLocal.Printf("Saving entity pos for %s: origin %s angles %s.\n", ent2->GetName(), ent->origin.ToString(), ent->angles.ToString() );

		// If the class has a model with shared data, manage this to avoid double frees
		if ( !lclass->pseudo )
		{
			// do nothing, the class model is a duplicate and can be freed
			if ( lclass->hModel )
			{
				// TODO: do not all this as we don't use shared data yet
				// gameLocal.m_ModelGenerator->FreeSharedModelData ( ent2->GetRenderEntity()->hModel );
			}
		}
		else
		{
			// deregister this static multi with us
			m_iNumStaticMulties --;

		}
		// gameLocal.Printf( "SEED %s: Culling entity #%i (%0.2f > %0.2f).\n", GetName(), i, deltaSq, lclass->cullDist );

		m_iNumExisting --;
		m_iNumVisible --;
		// add visible, reset exists, keep the others
		ent->flags += SEED_ENTITY_HIDDEN;
		ent->flags &= (! SEED_ENTITY_EXISTS);
		ent->entity = 0;

		// TODO: Do we need to use SafeRemove?
		ent2->PostEventMS( &EV_Remove, 0 );

		return true;
		}

	return false;
}

/*
================
Seed::Think
================
*/
void Seed::Think( void ) 
{
	struct seed_entity_t* ent;
	struct seed_class_t*  lclass;
	int culled = 0;
	int spawned = 0;

	// tels: seems unneccessary.
	// idEntity::Think();

	// for some reason disabling thinking doesn't work, so return early in case we have no targets
	// also return until activated
	if (m_iNumEntities < 0 || m_bWaitForTrigger)
	{
		return;
	}

	// haven't initialized entities yet?
	if (!m_bPrepared)
	{
		Prepare();
	}
	// GUI setting changed?
	if ( idMath::Fabs(cv_lod_bias.GetFloat() - m_fLODBias) > 0.1)
	{
		gameLocal.Printf ("SEED %s: GUI setting changed, recomputing.\n", GetName() );

		int cur_entities = m_iNumEntities;

		ComputeEntityCount();

		if (cur_entities != m_iNumEntities)
		{
			// TODO: We could keep the first N entities and only add the new ones if the quality
			// raises, and cull the last M entities if it sinks for more speed:
			Event_CullAll();

			// create same sequence again
			m_iSeed_2 = m_iOrgSeed;

			gameLocal.Printf ("SEED %s: Have now %i entities.\n", GetName(), m_iNumEntities );

			PrepareEntities();
			// save the new value
		}
		m_fLODBias = cv_lod_bias.GetFloat();
	}

	// After Restore(), do we need to do SetLODData()?
	if (m_bRestoreLOD && m_iNumStaticMulties > 0)
	{
		// go through all our entities and set things up
		int numEntities = m_Entities.Num();
		for (int i = 0; i < numEntities; i++)
		{
			ent = &m_Entities[i];
			lclass = &(m_Classes[ ent->classIdx ]);

			// tell the CStaticMulti entity that it should track updates:
			if (lclass->pseudo)
			{
				idEntity *ent2 = gameLocal.entities[ ent->entity ];

				CStaticMulti *sment = static_cast<CStaticMulti*>( ent2 );

				// Let the StaticMulti store the nec. data to create the combined rendermodel
				sment->SetLODData( lclass->m_LOD, lclass->modelname, &lclass->offsets, lclass->materialName, lclass->hModel );
					
				// enable thinking (mainly for debug draw)
				sment->BecomeActive( TH_THINK | TH_PHYSICS );
			}
		}
	}

	// Distance dependence checks
	if( (gameLocal.time - m_DistCheckTimeStamp) > m_DistCheckInterval )
	{
		m_DistCheckTimeStamp = gameLocal.time;

		// are we outside the player PVS?
		if ( m_iThinkCounter < 20 &&
			 ! gameLocal.pvs.InCurrentPVS( gameLocal.GetPlayerPVS(), m_iPVSAreas, m_iNumPVSAreas ) )
		{
			// if so, do nothing until think counter is high enough again
			//gameLocal.Printf( "SEED %s: Outside player PVS, think counter = %i, exiting.\n", GetName(), m_iThinkCounter );
			m_iThinkCounter ++;
			return;

			// TODO: cull all entities if m_iNumExisting > 0?
			// TODO: investigate if hiding brings us any performance, probably not, because
			//		 things outside the player PVS are not rendered anyway, but hiding/showing
			//		 them takes time.
		}

		m_iThinkCounter = 0;

		// cache these values for speed
		idVec3 playerOrigin = gameLocal.GetLocalPlayer()->GetPhysics()->GetOrigin();
		idVec3 vGravNorm = GetPhysics()->GetGravityNormal();
		float lodBias = cv_lod_bias.GetFloat();

		// square to avoid taking the square root from the distance
		lodBias *= lodBias;

		// for each of our "entities", do the distance check
		int numEntities = m_Entities.Num();
		for (int i = 0; i < numEntities; i++)
		{
			// TODO: let all auto-generated entities know about their new distance
			//		 so they can manage their attachment's LOD, too.

			// TODO: What to do about player looking thru spyglass?
			idVec3 delta = playerOrigin - m_Entities[i].origin;

			ent = &m_Entities[i];
			lclass = &(m_Classes[ ent->classIdx ]);

			// per class
			if( lclass->m_LOD && lclass->m_LOD->bDistCheckXYOnly )
			{
				delta -= (delta * vGravNorm) * vGravNorm;
			}

			// multiply with the user LOD bias setting, and cache that result:
			float deltaSq = delta.LengthSqr() / lodBias;

			// normal distance checks now
			if ( (ent->flags & SEED_ENTITY_EXISTS) == 0 && (lclass->spawnDist == 0 || deltaSq < lclass->spawnDist))
			{
				// Spawn and manage LOD, except for CStaticMulti entities with a megamodel,
				// these need to do their own LOD thinking:
				if (SpawnEntity( i, lclass->pseudo ? false : true ))
				{
					spawned ++;
				}
			}	
			else
			{
				// cull entities that are outside "hide_distance + fade_out_distance + cullRange
				if ( (ent->flags & SEED_ENTITY_EXISTS) != 0 && lclass->cullDist > 0 && deltaSq > lclass->cullDist)
				{
					// TODO: Limit number of entities to cull per frame
					if (CullEntity( i ))
					{
						culled ++;
					}

				}

/*				// TODO: Normal LOD code here (replicate from entity)
				// todo: int oldLODLevel = ent->lod;
				float fAlpha = ThinkAboutLOD( lclass->m_LOD, deltaSq );
				if (fAlpha == 0.0f)
				{
					// hide the entity
				}
				else
				{
					// if hidden, show the entity

					// if not combined entity
					// setAlpha
					// switchModel/switchSkin/noshadows
				}
*/
			}
		}
		if (spawned > 0 || culled > 0)
		{
			// the overall number seems to be the maximum number of entities that ever existed, so
			// to get the true real amount, one would probably go through gameLocal.entities[] and
			// count the valid ones:
			gameLocal.Printf( "%s: spawned %i, culled %i, existing: %i, visible: %i, overall: %i\n",
				GetName(), spawned, culled, m_iNumExisting, m_iNumVisible, gameLocal.num_entities );
		}
	}
}

/*
================
Seed::Event_Activate
================
*/
void Seed::Event_Activate( idEntity *activator ) {

	active = true;
	m_bWaitForTrigger = false;	// enough waiting around, lets do some action
	BecomeActive(TH_THINK);
}

/*
================
Seed::Event_Disable
================
*/
void Seed::Event_Disable( void ) {

	active = false;
	BecomeInactive(TH_THINK);
}

/*
================
Seed::Event_Enable
================
*/
void Seed::Event_Enable( void ) {

	active = true;
	BecomeInactive(TH_THINK);
}

/*
================
Seed::Event_CullAll
================
*/
void Seed::Event_CullAll( void ) {

	for (int i = 0; i < m_Entities.Num(); i++)
	{
		CullEntity( i );
	}
	// this should be unnec. but just to be safe:
	m_iNumStaticMulties = 0;
	m_iNumExisting = 0;
	m_iNumVisible = 0;
}
