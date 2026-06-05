//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_search_friendly_buildings.h
// Engineer moving to plausible observation points to discover friendly Engineer buildings

#ifndef TF_BOT_ENGINEER_SEARCH_FRIENDLY_BUILDINGS_H
#define TF_BOT_ENGINEER_SEARCH_FRIENDLY_BUILDINGS_H

#include "Path/NextBotPathFollow.h"

class CBaseObject;
class CObjectSentrygun;
class CObjectDispenser;
class CTFNavArea;

class CTFBotEngineerSearchFriendlyBuildings : public Action< CTFBot >
{
public:
	CTFBotEngineerSearchFriendlyBuildings( void );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );
	virtual void					OnEnd( CTFBot *me, Action< CTFBot > *nextAction );
	virtual ActionResult< CTFBot >	OnResume( CTFBot *me, Action< CTFBot > *interruptingAction );

	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );
	virtual EventDesiredResult< CTFBot > OnMoveToSuccess( CTFBot *me, const Path *path );
	virtual EventDesiredResult< CTFBot > OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason );

	virtual QueryResultType	ShouldHurry( const INextBot *me ) const;

	virtual const char *GetName( void ) const	{ return "EngineerSearchFriendlyBuildings"; };

private:
	enum SearchPhase
	{
		SEARCH_PHASE_TO_OBSERVATION_POINT = 0,
		SEARCH_PHASE_LOOK_AROUND,
		SEARCH_PHASE_SIDESTEP,
		SEARCH_PHASE_RETURN_HOME
	};

	SearchPhase m_phase;
	CHandle< CObjectSentrygun > m_homeSentry;

	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_giveUpTimer;
	CountdownTimer m_lookTimer;

	Vector m_searchGoal;
	int m_failedPathCount;

	bool MyImportantBuildingsNeedMe( CTFBot *me ) const;
	bool IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const;
	bool IsFriendlyBuildingCandidate( CTFBot *me, CBaseObject *obj ) const;
	bool IsVisibleBuildingForEngineer( CTFBot *me, CBaseObject *obj ) const;
	CBaseObject *FindVisibleFriendlyNestTarget( CTFBot *me ) const;
	bool SelectSearchGoal( CTFBot *me );
	bool SelectReturnHomeGoal( CTFBot *me );
	bool SelectSidestepGoal( CTFBot *me );
	void RecomputeSearchPath( CTFBot *me );
};

#endif // TF_BOT_ENGINEER_SEARCH_FRIENDLY_BUILDINGS_H
