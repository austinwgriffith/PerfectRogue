# Perfect Random NPC purge

The setup entries below were checked against the NTSC Final ROM. Character IDs are hexadecimal and local to each mission. The three stock AI commands for creating or cloning NPCs are blocked in Perfect Random. The “Runtime scripts” entries identify the stock encounters that use those commands.

Perfect Random still uses ordinary guard placements as input to its guard and objective rolls. Its reinforcements, allies, civilians and events spawn through the mod’s own logic.

### dataDyne Central: Defection

Cassandra De Vries, her secretary, and the programmer.

**Setup NPCs (3)**

- `CASSANDRA` · `24`
- `NEGOTIATOR` · `25`
- `OFFICEWORKER` · `26`

### dataDyne Research: Investigation

The K7 scientist, three other scientists, Dr. Caroll, and the shock-trooper actor.

**Setup NPCs (6)**

- `DD_LABTECH` · `35`, `36`, `37`, `38`
- `DRCAROLL` · `39`
- `DDSHOCK` · `3a`

**Runtime scripts** — dataDyne shock troopers at the alarm and uplink.

### dataDyne Central: Extraction

Dr. Caroll, the elevator shock-trooper actor, two technicians, Cassandra, Mr. Blonde, and three invincible escort actors.

**Setup NPCs (9)**

- `DRCAROLL` · `00`
- `DDSHOCK` · `0e`, `1e`
- `OFFICEWORKER` · `0f`, `10`
- `CASSANDRA` · `1c`
- `MRBLONDE` · `1d`
- `FEM_GUARD` · `1f`, `20`

### Carrington Villa: Hostage One

Carrington, the negotiator, two hostage takers, three hackers, and the CI soldier.

**Setup NPCs (8)**

- `DD_GUARD` · `4e`, `4f`
- `SECRETARY` · `50`
- `CARRINGTON` · `51`
- `DD_SHOCK_INF` · `52`, `53`, `54`
- `CISOLDIER` · `55`

### Chicago: Stealth

CIA contacts, FBI agents, the maintenance robot, and the three waiting G5 guard actors.

**Setup NPCs (11)**

- `G5_GUARD` · `11`, `12`, `1e`
- `FBIGUY` · `17`, `18`, `19`
- `CIAGUY` · `1a`, `1b`, `1c`, `1d`
- `CHICROB` · `1f`

**Runtime scripts** — G5 SWAT alarm responders.

### G5 Building: Reconnaissance

Cassandra, Mr. Blonde, Trent, the intro victim, and the striped intro guard.

**Setup NPCs (5)**

- `CASSANDRA` · `18`
- `TRENT` · `19`
- `STRIPES` · `1a`
- `MRBLONDE` · `1b`
- `G5_GUARD` · `1d`

**Runtime scripts** — G5 SWAT reinforcements.

### Area 51: Infiltration

The mechanic, lab technician and biotechnology staff, Jonathan and other intro actors, and ambush and clone templates.

**Setup NPCs (17)**

- `A51TROOPER` · `04`, `1f`, `20`, `21`, `22`, `32`, `34`
- `A51AIRMAN` · `23`, `24`, `25`, `31`, `33`
- `LABTECH` · `29`
- `BIOTECH` · `2a`, `2b`
- `OVERALL` · `30`
- `AREA51GUARD` · `35`

**Runtime scripts** — Airman squad, Jonathan in disguise, trooper reinforcements, and the alerted guard clone.

### Area 51: Rescue

Elvis, Jonathan and his cutscene escort, scientists, the mechanic, the doorman, and the preloaded lab and reinforcement groups.

**Setup NPCs (26)**

- `AREA51GUARD` · `2c`, `2d`, `2e`, `30`, `31`, `32`, `37`, `39`, `47`
- `BIOTECH` · `2f`, `33`, `34`, `40`, `41`
- `ELVIS1` · `38`
- `LABTECH` · `3a`, `3b`, `3c`, `3d`, `3e`, `3f`, `42`
- `OVERALL` · `43`
- `A51AIRMAN` · `44`, `48`, `49`

**Runtime scripts** — Warehouse guard clones, X-ray encounter clones, and dataDyne shock reinforcements.

### Area 51: Escape

Jonathan, Elvis, two idle technicians, the preloaded pursuit groups, and hidden biotechnology-room actors.

**Setup NPCs (33)**

- `AREA51GUARD` · `09`, `0a`, `0b`, `0c`, `0d`, `0e`, `0f`, `10`, `11`, `12`, `13`, `14`, `15`, `16`, `17`, `18`, `19`, `1a`, `1b`, `1c`, `1d`, `1e`, `1f`, `20`, `2b`
- `A51AIRMAN` · `21`
- `ELVIS1` · `22`
- `BIOTECH` · `23`, `24`, `25`, `26`, `27`, `28`

**Runtime scripts** — Area 51 pursuit guards and Jonathan follower-encounter clones.

### Air Base: Espionage

The undercover flight attendant, secretary, office staff, Trent, and checkpoint and cutscene actors.

**Setup NPCs (16)**

- `ALASKAN_GUARD` · `00`, `01`, `4b`, `4c`
- `STEWARDESS_COAT` · `38`, `48`
- `SECRETARY` · `39`
- `OFFICEWORKER` · `3a`, `3c`, `3d`
- `CIAGUY` · `44`
- `STEWARDESS` · `45`
- `STEWARD` · `46`
- `TRENT` · `47`
- `NSA_LACKEY` · `49`, `4a`

**Runtime scripts** — Foyer guard clones.

### Air Force One: Antiterrorism

Elvis, the President, pilots, stewards and coveralls crew, scripted cockpit security, Trent, Mr. Blonde, and the guard templates.

**Setup NPCs (19)**

- `THEKING` · `00`
- `OVERALL` · `01`, `04`, `05`
- `PRES_SECURITY` · `16`, `17`
- `PRESIDENT` · `18`
- `PILOTAF1` · `19`, `1a`
- `STEWARDESS` · `1b`
- `STEWARD` · `1c`
- `STRIPES` · `30`, `35`, `36`, `37`
- `TRENT` · `31`, `32`
- `MRBLONDE` · `33`, `34`

**Runtime scripts** — Mr. Blonde’s guard clones.

### Crash Site: Confrontation

Elvis, the President and his clone, Trent, Mr. Blonde’s transformation actors, four robots, and the guard spawn templates.

**Setup NPCs (15)**

- `THEKING` · `00`, `38`
- `TRENT` · `01`, `02`
- `MRBLONDE` · `03`
- `SKEDAR` · `04`
- `PRESIDENT` · `05`, `06`
- `PRESIDENT_CLONE2` · `08`
- `CHICROB` · `26`, `27`, `28`, `29`
- `STRIPES` · `30`, `31`

**Runtime scripts** — dataDyne guard reinforcements and guard clones.

### Pelagic II: Exploration

Elvis, his CI escort, three pilots, and three lab technicians. Dormant Pelagic guards enter the Perfect Random guard pool.

**Setup NPCs (8)**

- `THEKING` · `00`
- `PELAGIC_GUARD` · `02`
- `PILOTAF1` · `3c`, `3e`, `3f`
- `DD_LABTECH` · `4e`, `4f`, `50`

### Deep Sea: Nullify Threat

Elvis, Dr. Caroll, Mr. Blonde’s outro actors, the sniper double, the dead Skedar, and preloaded Skedar and Pelagic encounter actors.

**Setup NPCs (29)**

- `THEKING` · `00`
- `SKEDAR` · `21`
- `MRBLONDE` · `22`, `23`
- `DRCAROLL` · `24`
- `MINISKEDAR` · `25`, `26`, `27`, `28`, `29`, `2a`, `2b`, `30`, `31`, `32`, `33`, `34`, `35`, `38`, `39`, `3a`, `3b`, `41`, `42`
- `PELAGIC_GUARD` · `3c`, `3d`, `3e`, `3f`
- `DDSNIPER` · `40`

**Runtime scripts** — Mini-Skedar clones and waves in the megaweapon, circular, and dead-end rooms.

### Carrington Institute: Defense

Carrington, Jonathan, Institute hostages and friendly soldiers, hostage takers, shock-trooper spawners, and cutscene Skedar and Mr. Blonde actors.

**Setup NPCs (42)**

- `CARREVENINGSUIT` · `00`
- `CIFEMTECH` · `01`, `04`, `08`
- `CILABTECH` · `02`, `03`, `05`, `06`, `07`
- `DDSHOCK` · `0c`, `0d`, `0e`, `10`, `12`, `13`, `14`, `24`, `25`, `26`, `27`
- `CISOLDIER` · `15`, `16`, `17`, `18`, `19`, `1a`, `1b`, `1c`, `1d`, `1e`, `38`
- `MRBLONDE` · `28`, `29`, `2a`, `2b`, `2c`, `2d`, `30`
- `SKEDAR` · `31`, `32`, `33`, `34`

**Runtime scripts** — dataDyne shock trooper clones and three holographic hostage doubles.

### Attack Ship: Covert Assault

Elvis, the Maians, Cassandra, the idle Skedar actors, and hangar and bridge spawn templates.

**Setup NPCs (13)**

- `ELVISWAISTCOAT` · `00`
- `SKEDAR` · `02`, `07`, `08`, `09`, `0a`, `31`, `32`, `34`
- `ELVIS1` · `2e`, `2f`, `30`
- `CASSANDRA` · `33`

**Runtime scripts** — Hangar and bridge Skedar clones.

### Skedar Ruins: Battle Shrine

Elvis, the hidden Skedar King and outro cast, and the puzzle room and King spawn templates.

**Setup NPCs (16)**

- `MINISKEDAR` · `23`, `24`, `25`, `26`, `28`, `29`, `2a`, `35`
- `SKEDARKING` · `33`, `3b`
- `SKEDAR` · `34`, `37`, `38`, `39`, `3a`
- `ELVISWAISTCOAT` · `36`

**Runtime scripts** — Cloaked Skedar, Mini-Skedar, and the King’s Skedar and Mini-Skedar clones.

### Mr. Blonde’s Revenge

Cassandra and the security chief.

**Setup NPCs (2)**

- `FEM_GUARD` · `21`
- `CASSANDRA` · `22`

### Maian SOS

The scientist actors, the outro guard, and Area 51 airman clone templates.

**Setup NPCs (8)**

- `LABTECH` · `44`, `45`, `56`
- `A51AIRMAN` · `4e`, `4f`, `50`, `51`, `52`

**Runtime scripts** — Area 51 guard and airman clones.

### War

The Maian leader, Maian soldiers, and the hidden Maian and Skedar spawn templates. The Kings and their guard enter the Perfect Random guard pool.

**Setup NPCs (8)**

- `ELVIS1` · `3d`, `44`, `45`, `46`, `47`, `48`
- `CISOLDIER` · `3e`, `3f`

**Runtime scripts** — Maian reinforcements and the three Skedar Kings’ Skedar waves.

### The Duel

The guard, Jonathan, and Trent are the entire setup guard pool and use Perfect Random combat and objective logic.

### Retaking the Institute (unused setup)

The stock setup contains no NPCs.

## Shared spawns

- The global AI lists can attempt guard clones when a guard hears the player or an alarm. Those attempts use the same blocked clone command.
- The vanilla co-op buddy spawner is gated off when Perfect Random is active. Perfect Random’s own buddy logic can still create allies.

The stock ROM audit recorded **294** purged setup entries. Startup smoke tests reached the first Perfect Random objective target on all 21 playable missions. The unused Retaking the Institute setup was checked separately.
