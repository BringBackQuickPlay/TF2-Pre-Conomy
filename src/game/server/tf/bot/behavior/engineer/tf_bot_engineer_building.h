//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_building.h
// At building location, constructing buildings
// Michael Booth, May 2010

#ifndef TF_BOT_ENGINEER_BUILDING_H
#define TF_BOT_ENGINEER_BUILDING_H

class CTFBotHintSentrygun;
class CBaseObject;
class CObjectSentrygun;
class CObjectDispenser;
class CObjectTeleporter;

class CTFBotEngineerBuilding : public Action< CTFBot >
{
public:
	CTFBotEngineerBuilding( void );
	CTFBotEngineerBuilding( CTFBotHintSentrygun *sentryBuildHint );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );
	virtual void					OnEnd( CTFBot *me, Action< CTFBot > *nextAction );

	virtual ActionResult< CTFBot >	OnResume( CTFBot *me, Action< CTFBot > *interruptingAction );

	virtual EventDesiredResult< CTFBot > OnTerritoryLost( CTFBot *me, int territoryID );
	virtual EventDesiredResult< CTFBot > OnTerritoryCaptured( CTFBot *me, int territoryID );

	virtual const char *GetName( void ) const	{ return "EngineerBuilding"; };

private:
	CountdownTimer m_searchTimer;
	CountdownTimer m_getAmmoTimer;
	CountdownTimer m_repathTimer;
	CountdownTimer m_buildTeleporterExitTimer;

	int m_sentryTriesLeft;

	CountdownTimer m_dispenserRetryTimer;
	CountdownTimer m_teleportExitRetryTimer;

	PathFollower m_path;

	enum FriendlyBuildingTask
	{
		FRIENDLY_BUILDING_TASK_NONE = 0,
		FRIENDLY_BUILDING_TASK_PATROL_TO_FRIENDLY,
		FRIENDLY_BUILDING_TASK_PATROL_WAIT,
		FRIENDLY_BUILDING_TASK_RETURN_HOME,
		FRIENDLY_BUILDING_TASK_PATROL_FEINT,
		FRIENDLY_BUILDING_TASK_UPGRADE,
		FRIENDLY_BUILDING_TASK_DEFEND,
		FRIENDLY_BUILDING_TASK_SPYCHECK
	};

	FriendlyBuildingTask m_friendlyBuildingTask;
	CHandle< CBaseObject > m_friendlyBuildingTarget;

	CountdownTimer m_friendlyBuildingRepathTimer;
	CountdownTimer m_friendlyBuildingTaskTimer;
	CountdownTimer m_nextFriendlyPatrolTimer;
	CountdownTimer m_nextFriendlyUpgradeTimer;
	CountdownTimer m_nextFriendlySpyCheckTimer;
	CountdownTimer m_spyCheckShotTimer;

	int m_friendlyBuildingStartLevel;
	int m_spyCheckShotsRemaining;

	Vector m_friendlyPatrolFeintGoal;
	CHandle< CBaseObject > m_friendlyPatrolFeintReturnTarget;
	int m_spyKilledPatrolBoostCount;

	CHandle< CTFBotHintSentrygun > m_sentryBuildHint;

	bool m_hasBuiltSentry;

	enum NearbyMetalType
	{
		NEARBY_METAL_UNKNOWN,
		NEARBY_METAL_NONE,
		NEARBY_METAL_EXISTS
	};

	NearbyMetalType m_nearbyMetalStatus;

	CountdownTimer m_territoryRangeTimer;
	bool m_isSentryOutOfPosition;
	bool CheckIfSentryIsOutOfPosition( CTFBot *me ) const;

	bool TryAttackMiniSentryTarget( CTFBot *me, CObjectSentrygun *mySentry );

	bool PatrolBetweenMyAndFriendlyBuildings( CTFBot *me );
	bool UpgradeFriendlyBuildings( CTFBot *me );
	bool DefendFriendlyBuildings( CTFBot *me );
	bool SpyCheckBuildings( CTFBot *me );

	bool MyImportantBuildingsRecentlyAttacked( CTFBot *me, float time ) const;
	bool IsFriendlyBuildingCandidate( CTFBot *me, CBaseObject *obj ) const;
	bool IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const;
	bool CanSeeBuilding( CTFBot *me, CBaseObject *obj ) const;

	CBaseObject *FindVisibleFriendlyPatrolBuilding( CTFBot *me ) const;
	float GetRandomizedPatrolDuration( float minTime, float maxTime ) const;
	bool BeginPatrolFeint( CTFBot *me, CBaseObject *realGoal, CBaseObject *returnTarget );
	CBaseObject *FindClosestFriendlyPlayerForSpyCheck( CTFBot *me ) const;

	CBaseObject *FindVisibleFriendlyUpgradeBuilding( CTFBot *me ) const;
	CObjectSentrygun *FindFriendlySentryNeedingDefense( CTFBot *me ) const;
	CBaseObject *FindSpyCheckBuilding( CTFBot *me ) const;

	void ReturnToOwnSentry( CTFBot *me );
	void WorkOnBuilding( CTFBot *me, CBaseObject *target, const char *reason );

	void UpgradeAndMaintainBuildings( CTFBot *me );
	bool IsMetalSourceNearby( CTFBot *me ) const;
};


#endif // TF_BOT_ENGINEER_BUILDING_H
