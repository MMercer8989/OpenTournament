// Copyright (c) 2019-2020 Open Tournament Project, All Rights Reserved.

/////////////////////////////////////////////////////////////////////////////////////////////////

#include "UR_GameMode.h"

#include "EngineUtils.h"    // for TActorIterator<>
#include "Kismet/GameplayStatics.h"

#include "UR_Character.h"
#include "UR_GameState.h"
#include "UR_InventoryComponent.h"
#include "UR_LocalMessage.h"
#include "UR_PlayerController.h"
#include "UR_PlayerState.h"
#include "UR_Projectile.h"
#include "UR_Weapon.h"
#include "UR_Ammo.h"
#include "UR_Widget_ScoreboardBase.h"
#include "UR_TeamInfo.h"
#include "GameFramework/Controller.h"

//include statements so we will have access to functions for printing text on screen
#include <EngineGlobals.h>
#include <Runtime/Engine/Classes/Engine/Engine.h>
#include <string>
#include <stdlib.h>

/////////////////////////////////////////////////////////////////////////////////////////////////

AUR_GameMode::AUR_GameMode()
{
    ScoreboardClass = UUR_Widget_ScoreboardBase::StaticClass();
    DeathMessageClass = UUR_LocalMessage::StaticClass();

    GoalScore = 10;
    TimeLimit = 300;
    OvertimeExtraTime = 120;

    MaxPlayers = 10;
    NumTeams = 0;
    TeamsFillMode = ETeamsFillMode::Even;

    SelfDamage = 1.f;
    TeamDamageDirect = 0.f;
    TeamDamageRetaliate = 0.f;
}

void AUR_GameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
    Super::InitGame(MapName, Options, ErrorMessage);

    GoalScore = UGameplayStatics::GetIntOption(Options, TEXT("GoalScore"), GoalScore);
    TimeLimit = UGameplayStatics::GetIntOption(Options, TEXT("TimeLimit"), TimeLimit);
    OvertimeExtraTime = UGameplayStatics::GetIntOption(Options, TEXT("OvertimeExtraTime"), OvertimeExtraTime);
    MaxPlayers = UGameplayStatics::GetIntOption(Options, TEXT("MaxPlayers"), MaxPlayers);
    NumTeams = UGameplayStatics::GetIntOption(Options, TEXT("NumTeams"), NumTeams);
    TeamsFillMode = UGameplayStatics::ParseOption(Options, TEXT("TeamsFillMode"));
    SelfDamage = UUR_FunctionLibrary::GetFloatOption(Options, TEXT("SelfDamage"), SelfDamage);
    TeamDamageDirect = UUR_FunctionLibrary::GetFloatOption(Options, TEXT("TeamDamageDirect"), TeamDamageDirect);
    TeamDamageRetaliate = UUR_FunctionLibrary::GetFloatOption(Options, TEXT("TeamDamageRetaliate"), TeamDamageRetaliate);

    if (NumTeams > 0)
    {
        DesiredTeamSize = FMath::CeilToInt((float)MaxPlayers / (float)NumTeams);
    }
    else
    {
        NumTeams = 0;
        DesiredTeamSize = 1;
    }
}

void AUR_GameMode::InitGameState()
{
    Super::InitGameState();

    if (AUR_GameState* GS = GetGameState<AUR_GameState>())
    {
        for (auto Team : GS->Teams)
        {
            Team->Destroy();
        }
        GS->Teams.Empty();

        // Only create 2 teams to start with, others will be added as they fill up.
        for (int32 i = 0; i < FMath::Min(NumTeams, 2); i++)
        {
            GS->AddNewTeam();
        }
    }
}

void AUR_GameMode::GenericPlayerInitialization(AController* C)
{
    Super::GenericPlayerInitialization(C);

    if (C && C->PlayerState && !C->PlayerState->IsOnlyASpectator())
    {
        if (AUR_PlayerState* PS = Cast<AUR_PlayerState>(C->PlayerState))
        {
            if (NumTeams > 0)
            {
                AssignDefaultTeam(PS);
            }
        }
    }
}

void AUR_GameMode::AssignDefaultTeam(AUR_PlayerState* PS)
{
    if (NumTeams == 0)
    {
        IUR_TeamInterface::Execute_SetTeamIndex(PS, -1);
        return;
    }

    if (NumTeams == 1)
    {
        IUR_TeamInterface::Execute_SetTeamIndex(PS, 0);
        return;
    }

    if (AUR_GameState* GS = GetGameState<AUR_GameState>())
    {
        bool bCreateNewTeam = false;

        if (TeamsFillMode.Equals(ETeamsFillMode::Squads))
        {
            // Create a new squad only if all existing squads are full
            bCreateNewTeam = true;
            for (const auto Team : GS->Teams)
            {
                if (Team->Players.Num() < DesiredTeamSize)
                {
                    bCreateNewTeam = false;
                    break;
                }
            }
        }
        else
        {
            // Create a new team if we don't have NumTeams yet
            if (GS->Teams.Num() < NumTeams)
            {
                bCreateNewTeam = true;
                // Unless one existing team has 0 players
                for (const auto Team : GS->Teams)
                {
                    if (Team->Players.Num() == 0)
                    {
                        bCreateNewTeam = false;
                        break;
                    }
                }
            }
        }

        if (bCreateNewTeam)
        {
            GS->AddNewTeam();
        }

        // Fit player into the smallest team

        AUR_TeamInfo* SmallestTeam = GS->Teams[0];
        for (const auto Team : GS->Teams)
        {
            if (Team->Players.Num() < SmallestTeam->Players.Num())
            {
                SmallestTeam = Team;
            }
        }

        SmallestTeam->AddPlayer(PS);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////
// Match
/////////////////////////////////////////////////////////////////////////////////////////////////

void AUR_GameMode::HandleMatchHasStarted()
{
    Super::HandleMatchHasStarted();

    AUR_GameState* GS = GetGameState<AUR_GameState>();
    if (GS)
    {
        if (TimeLimit > 0)
        {
            GS->SetTimeLimit(TimeLimit);
            GS->OnTimeUp.AddUniqueDynamic(this, &AUR_GameMode::OnMatchTimeUp);
        }
        else
        {
            GS->ResetClock();
        }
    }
}

void AUR_GameMode::OnMatchTimeUp_Implementation(AUR_GameState* GS)
{
    // Unbind match delegate
    GS->OnTimeUp.RemoveDynamic(this, &AUR_GameMode::OnMatchTimeUp);

    if (!CheckEndGame(nullptr))
    {
        // Overtime

        //TODO: msg class
        BroadcastLocalized(this, UUR_LocalMessage::StaticClass(), 0, nullptr, nullptr, GS);

        if (OvertimeExtraTime > 0)
        {
            GS->SetTimeLimit(OvertimeExtraTime);
            GS->OnTimeUp.AddUniqueDynamic(this, &AUR_GameMode::OnMatchTimeUp);
        }
        else
        {
            // Start counting up
            GS->ResetClock();
        }
    }
}

void AUR_GameMode::SetPlayerDefaults(APawn* PlayerPawn)
{
    if (AUR_Character* URCharacter = Cast<AUR_Character>(PlayerPawn))
    {
        //NOTE: Technically RestartPlayer() supports restarting a player that is not dead.
        // In that case, the existing Pawn is not touched, but this method is still called.
        // I'm not sure if we should try to support that.
        if (URCharacter->InventoryComponent)
        {
            URCharacter->InventoryComponent->Clear();

            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            SpawnParams.Owner = URCharacter;
            SpawnParams.Instigator = URCharacter;
            for (const FStartingWeaponEntry& Entry : StartingWeapons)
            {
                AUR_Weapon* StartingWeapon = GetWorld()->SpawnActor<AUR_Weapon>(Entry.WeaponClass, URCharacter->GetActorLocation(), URCharacter->GetActorRotation(), SpawnParams);
                if (StartingWeapon)
                {
                    StartingWeapon->GiveTo(URCharacter);
                    //TODO: Weapons with multiple ammo classes
                    if (StartingWeapon->AmmoRefs.Num() > 0 && StartingWeapon->AmmoRefs[0])
                    {
                        StartingWeapon->AmmoRefs[0]->SetAmmoCount(Entry.Ammo);
                    }
                }
            }
        }
    }
    Super::SetPlayerDefaults(PlayerPawn);
}


/////////////////////////////////////////////////////////////////////////////////////////////////
// Damage & Kill
/////////////////////////////////////////////////////////////////////////////////////////////////

void AUR_GameMode::ModifyDamage_Implementation(float& Damage, float& KnockbackPower, AUR_Character* Victim, AController* DamageInstigator, const FDamageEvent& DamageEvent, AActor* DamageCauser)
{
    if (Victim && DamageInstigator && Victim->GetController() == DamageInstigator)
    {
        Damage *= SelfDamage;
        // Full self-knockback
    }

    //NOTE: Using InstigatorController to check team is not ideal because controller can change team.
    // Character isn't any better because it can die during projectile travel time.
    // For projectiles, we should assign a team to the projectile on spawn, and fetch that upon damage.

    if (Victim && Victim->GetController() != DamageInstigator && IUR_TeamInterface::Execute_IsAlly(Victim, DamageInstigator))
    {
        const float Retaliate = TeamDamageRetaliate * Damage;
        // Retaliation counts as self damage
        if (Retaliate > 0.f && SelfDamage > 0.f)
        {
            if (APawn* InstigatorPawn = DamageInstigator->GetPawn())
            {
                InstigatorPawn->TakeDamage(Retaliate, FDamageEvent(), DamageInstigator, DamageCauser);
            }
        }

        Damage *= TeamDamageDirect;
        KnockbackPower *= TeamDamageDirect;
    }
}

bool AUR_GameMode::PreventDeath_Implementation(AController* Killed, AController* Killer, const FDamageEvent& DamageEvent, AActor* DamageCauser)
{
    return false;
}

/**
* Things to do and to see how to structure the whole thing :
*
* - kills, deaths, suicides, message   => the message could be sent from PS->AddKill / AddSuicide ?
*
* - multi kills, counting & message    => can do in PS->AddKill(Victim)
* - sprees, counting & message         => can do in PS->AddKill(Victim)
* - ending someone's spree             => can do in PS->AddDeath(Killer)
* - first blood ?
* - revenge ?                          => can do in PS->AddKill(Victim)
*
* - stats for every single type of damage
*
* - update score based on kill ? this is heavily gamemode-dependent
* - check end game ? again gamemode-dependent.
* 
* In case of DM, Score = Kills-Suicides.
* In case of CTF, Score = Caps.
* Also need to update TeamScores somehow.
* This could all fit in PS->AddKill / AddSuicide,
* But ideally we also want to avoid having extra PS subclasses whenever possible.
*
* We have to be careful as Blueprints cannot choose specifically which Super() method to call.
*/

void AUR_GameMode::DeathMessage(AController* Victim, AController* Killer, const FDamageEvent& DamageEvent, AActor* DamageCauser) {

    //set the local players name (this will be attached to the front of most death messages)
    FString playerName = "player"; //placeholder for now
    FString victimName;
    FString killerName;

    //make sure the Victim and Killer are not null first
    //on the offchance that they are null just use a placeholder name
    if (Victim != nullptr && Killer != nullptr) {
        victimName = Victim->GetName();
        killerName = Killer->GetName();
    }
    else {
        victimName = "Unknown_Victim";
        killerName = "Unknown_Killer";
    }

    FString suicideMessage[5][5] = {
        {" blew themselves to bits", " got too close to the rockets", "'s head has been blown apart", " is now a smoldering pile of viscera", " has been GIBBED... by their own doing"}, //Rocket launcher Suicide messages
        {" couldn't handle the flac cannon", ", flac cannons hurt dont they", " took a facefull of shrapnel", " has transitioned to swiss cheese"}, //Flac cannon suicide messages
        {" has electrified themself", " has experienced unlimited power...", " has rode the lightning", " vaporized themself"}, //plasma gun suicide messages
        {" just played around with their own grenade", " fragged themself", " thought it would be best to blow their arms off", " decided to eat a live grenade"}, //grenade launcher suicide messages
        {" just gave up", " couldn't handle life", " said 'right, that's enough...'", " has had enough"} //generic suicide messages
    };

    FString StandardKillMessage[5][5] = {
        {" Just got fragged by ", " Just got their skull cleaved by a passing rocket thanks to ", " just got lit up by ", " couldn't outrun the rockets from ", " has been GIBBED by "}, //Rocket Launcher
        {" ate some flac, courtesy of ", " took on the flac cannon and lost thanks to ", " got filled full of holes by ", " just got dumpstered thanks to ", " has become swiss cheese thanks to "}, //Flac Cannon
        {" has just been discombobulated by ", " has just been fried by ", " has been burnt to a crisp by ", " has been vaporized by ", " is now a smoldering pile of ash thanks to "}, //Plasma gun
        {" tripped over a grenade placed by ", " was dismembered thanks to ", " was ripped apart by a grenade thanks to ", " is now suffering from full body failure thanks to ", " has been bombed by "}, //grenade launcher
        {" just got fragged by ", " has been taken down by ", " has been filled full of hot lead by ", " has been killed by ", " has suffered defeat at the hands of "} //generic

    };

    FString MiscKillMessage[5] = {" has been killed... by the arena, ouch", " has been fragged... by the stage?", " has died", " has been discombobulated", " has been rendered helpless by the arena"};

    //here we will place the death messages to be output to the screen, these should be split into a few different categories
    //depending on who the killer is

    if (!Killer || Killer == Victim) {
        //suicide messages go here
        if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_Rocket")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(playerName.Append(suicideMessage[0][rand() % 4 + 0])));
        }
        else if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_Shotgun")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(playerName.Append(suicideMessage[1][rand() % 4 + 0])));
        }
        else if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_CannonBall")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(playerName.Append(suicideMessage[1][rand() % 4 + 0])));
        }
        else if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_EnergyBall")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(playerName.Append(suicideMessage[2][rand() % 4 + 0])));
        }
        else if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_Grenade")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(playerName.Append(suicideMessage[3][rand() % 4 + 0])));
        }
        else {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(playerName.Append(suicideMessage[4][rand() % 4 + 0])));
        }
    }
    else if (Killer && Killer != Victim) {

        //regular kills
        if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_Rocket")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(victimName.Append(StandardKillMessage[0][rand() % 4 + 0]).Append(killerName)));
        }
        else if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_Shotgun")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(victimName.Append(StandardKillMessage[1][rand() % 4 + 0]).Append(killerName)));
        }
        else if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_CannonBall")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(victimName.Append(StandardKillMessage[1][rand() % 4 + 0]).Append(killerName)));
        }
        else if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_EnergyBall")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(victimName.Append(StandardKillMessage[2][rand() % 4 + 0]).Append(killerName)));
        }
        else if (DamageCauser->GetActorLabel().Contains("BP_UR_Projectile_Grenade")) {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(victimName.Append(StandardKillMessage[3][rand() % 4 + 0]).Append(killerName)));
        }
        else {
            GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(victimName.Append(StandardKillMessage[4][rand() % 4 + 0]).Append(killerName)));
        }
    }
    else {
        //other
        GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Red, FString(playerName.Append(MiscKillMessage[rand() % 4 + 0])));
    }
}

void AUR_GameMode::CoverageTestDM(AController* Victim, AController* Killer, const FDamageEvent& DamageEvent, AActor* DamageCauser) {

    const FString dmgCauserArr[6] = { "BP_UR_Projectile_Rocket", "BP_UR_Projectile_Shotgun", "BP_UR_Projectile_CannonBall", "BP_UR_Projectile_EnergyBall", "BP_UR_Projectile_Grenade", "Stage_DMG"};

    //first tests: the killer is also the victim
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("Starting DeathMessage coverage tests"));
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("**********************************************"));
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("Test 1: A player is killed by their own attack"));
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("**********************************************"));
    for (int i = 0; i < 6; i++) {
        DamageCauser->SetActorLabel(dmgCauserArr[i]); //here we will go through the different damage types
        DeathMessage(Victim, Victim, DamageEvent, DamageCauser);
    }

    //second tests: The killer is not the victim
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("**********************************************"));
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("Test 2: A player is killed by someone else"));
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("**********************************************"));
    for (int i = 0; i < 6; i++) {
        DamageCauser->SetActorLabel(dmgCauserArr[i]); //here we will go through the different damage types
        DeathMessage(Killer, Victim, DamageEvent, DamageCauser);
    }

    //third test: stage deaths
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("**********************************************"));
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("Test 3: The player is killed by the arena"));
    GEngine->AddOnScreenDebugMessage(-1, 20.0f, FColor::Blue, FString("**********************************************"));
    Killer = nullptr;
    for (int i = 0; i < 6; i++) {
        DamageCauser->SetActorLabel(dmgCauserArr[i]); //here we will go through the different damage types
        DeathMessage(Killer, Victim, DamageEvent, DamageCauser);
    }

}

void AUR_GameMode::PlayerKilled_Implementation(AController* Victim, AController* Killer, const FDamageEvent& DamageEvent, AActor* DamageCauser)
{
    CoverageTestDM(Victim, Killer, DamageEvent, DamageCauser);//initiate the covrage test
    //DeathMessage(Victim, Killer, DamageEvent, DamageCauser); //call a function to display the public death messages
    RegisterKill(Victim, Killer, DamageEvent, DamageCauser); 
}

void AUR_GameMode::RegisterKill(AController* Victim, AController* Killer, const FDamageEvent& DamageEvent, AActor* DamageCauser)
{
    //the death messages mentioned in here seem only to apply to the scoreboard
    if (Victim)
    {
        AUR_PlayerState* VictimPS = Victim->GetPlayerState<AUR_PlayerState>();

        if (VictimPS)
        {
            VictimPS->AddDeath(Killer);
        }

        if (Killer && Killer != Victim)
        {
            AUR_PlayerState* KillerPS = Killer->GetPlayerState<AUR_PlayerState>();
            if (KillerPS)
            {
                KillerPS->AddKill(Victim);
            }
            BroadcastLocalized(Killer, DeathMessageClass, 0, Victim->GetPlayerState<APlayerState>(), Killer->GetPlayerState<APlayerState>(), DamageEvent.DamageTypeClass);
        }
        else
        {
            if (VictimPS)
            {
                VictimPS->AddSuicide();
            }
            BroadcastLocalized(Killer, DeathMessageClass, 1, Victim->GetPlayerState<APlayerState>(), nullptr, DamageEvent.DamageTypeClass);
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////
// End Game
/////////////////////////////////////////////////////////////////////////////////////////////////

/**
* Trying to stick with the engine game framework provided.
* However I don't like how they use Tick to check ReadyToEndMatch().
* I prefer event-based here, calling CheckEndGame whenever something happens.
*/

bool AUR_GameMode::CheckEndGame(AActor* Focus)
{
    AActor* Winner = IsThereAWinner();
    if (Winner)
    {
        if (Focus)
        {
            // Passed in focus may be a controller or a PS. Try to resolve to a pawn.
            Focus = ResolveEndGameFocus(Focus);
        }
        else
        {
            // Winner may be a controller, PS, or even a team. Try to resolve to a pawn.
            Focus = ResolveEndGameFocus(Winner);
        }

        TriggerEndMatch(Winner, Focus);
        return true;
    }
    return false;
}

AActor* AUR_GameMode::IsThereAWinner_Implementation()
{
    // By default return the highest scorer..?
    AUR_GameState* GS = GetGameState<AUR_GameState>();
    if (GS)
    {
        if (GS->Teams.Num() > 1)
        {
            AUR_TeamInfo* BestTeam = NULL;
            bool bTie = false;
            for (AUR_TeamInfo* Team : GS->Teams)
            {
                if (!BestTeam || Team->GetScore() > BestTeam->GetScore())
                {
                    BestTeam = Team;
                    bTie = false;
                }
                else if (Team->GetScore() == BestTeam->GetScore())
                {
                    bTie = true;
                }
            }
            if (!bTie)
            {
                return BestTeam;
            }
        }
        else
        {
            APlayerState* BestPlayer = NULL;
            bool bTie = false;
            for (APlayerState* PS : GS->PlayerArray)
            {
                if (!BestPlayer || PS->GetScore() > BestPlayer->GetScore())
                {
                    BestPlayer = PS;
                    bTie = false;
                }
                else if (PS->GetScore() == BestPlayer->GetScore())
                {
                    bTie = true;
                }
            }
            if (!bTie)
            {
                return BestPlayer;
            }
        }
    }

    //NOTE: maybe we should put the DM implem here because it kind of makes sense.
    // GoalScore is part of this class so we should have a default handling here.

    return nullptr;
}

AActor* AUR_GameMode::ResolveEndGameFocus_Implementation(AActor* Winner)
{
    if (APlayerState* PS = Cast<APlayerState>(Winner))
    {
        return PS->GetPawn();
    }
    if (AController* C = Cast<AController>(Winner))
    {
        return C->GetPawn();
    }
    if (AUR_TeamInfo* Team = Cast<AUR_TeamInfo>(Winner))
    {
        AUR_PlayerState* Top = NULL;
        for (AUR_PlayerState* PS : Team->Players)
        {
            if (PS && PS->GetPawn() && (!Top || PS->GetScore() > Top->GetScore()))
            {
                Top = PS;
            }
        }
        if (Top)
        {
            return Top->GetPawn();
        }
    }
    return Winner;
}

void AUR_GameMode::TriggerEndMatch_Implementation(AActor* Winner, AActor* Focus)
{
    if (AUR_GameState* GS = GetGameState<AUR_GameState>())
    {
        GS->Winner = Winner;
        GS->EndGameFocus = Focus;
    }
    SetMatchState(MatchState::WaitingPostMatch);
}

void AUR_GameMode::HandleMatchHasEnded()
{
    Super::HandleMatchHasEnded();

    // Freeze the game
    //TODO: this is probably the wrong way to do it
    for (TActorIterator<AActor> ActorIt(GetWorld()); ActorIt; ++ActorIt)
    {
        AActor* Actor = *ActorIt;
        if (Cast<APawn>(Actor) || Cast<AUR_Projectile>(Actor))
        {
            Actor->CustomTimeDilation = 0.01f;
        }
    }

    AUR_GameState* GS = GetGameState<AUR_GameState>();

    // Set focus - if null, everyone will continue viewing their current view target.
    for (FConstPlayerControllerIterator Iterator = GetWorld()->GetPlayerControllerIterator(); Iterator; ++Iterator)
    {
        if (APlayerController* PC = Iterator->Get())
        {
            AActor* VT = (GS && GS->EndGameFocus) ? GS->EndGameFocus : PC->GetViewTarget();
            PC->UnPossess();
            PC->SetViewTarget(VT);
        }
    }

    AnnounceWinner(GS ? GS->Winner : nullptr);

    if (GS)
    {
        GS->OnTimeUp.RemoveDynamic(this, &AUR_GameMode::OnMatchTimeUp);
        //NOTE: This system is a bit dangerous, we might change state completely and forget some TimeUp handlers.
        // It might be a good thing to clear all listeners whenever GS->SetTimeLimit is called.
        // Or something like that.
        GS->SetTimeLimit(10);
        GS->OnTimeUp.AddUniqueDynamic(this, &AUR_GameMode::OnEndGameTimeUp);
    }
}

void AUR_GameMode::AnnounceWinner_Implementation(AActor* Winner)
{
    APlayerState* WinnerPS = nullptr;
    if (APlayerState* PS = Cast<APlayerState>(Winner))
    {
        WinnerPS = PS;
    }
    else if (APawn* P = Cast<APawn>(Winner))
    {
        WinnerPS = P->GetPlayerState();
    }
    else if (AController* C = Cast<AController>(Winner))
    {
        WinnerPS = C->GetPlayerState<APlayerState>();
    }

    //TODO: msg class
    BroadcastLocalized(Winner, UUR_LocalMessage::StaticClass(), 0, WinnerPS, nullptr, Winner);
}

void AUR_GameMode::OnEndGameTimeUp(AUR_GameState* GS)
{
    if (GetNetMode() != NM_DedicatedServer)
    {
        AUR_PlayerController* LocalPC = GetWorld()->GetFirstPlayerController<AUR_PlayerController>();
        if (LocalPC)
        {
            LocalPC->ReturnToMainMenu();
            return;
        }
    }

    if (GetWorld()->IsPlayInEditor())
    {
        // Cannot RestartGame in PIE dedicated server for some reason.
        // Dunno what to do, stay stuck in endgame forever.
        return;
    }

    // else
    RestartGame();
}
