# Mooncake master HA additions

This build adds native batch OpLog snapshots and qualified fixes on the pinned
Mooncake source. Engines and SSD owners keep their separately pinned images.
Build with `phala/ha/Dockerfile`; release acceptance requires final-image tests.

Set `MC_MASTER_PRIORITY` only on the master process to a unique positive uint32
node number (101, 102, ...). Lower numbers have precedence among registered,
caught-up candidates. An absent setting retains native unordered election.
Do not mix priority-enabled and unordered masters within one cluster.

Registration uses a leased etcd key. One atomic etcd transaction checks that the
candidate's registration still belongs to it, no lower registration exists, and
no leader exists. Losing quorum cannot create a second writable leader. Existing
leadership fencing and final recovery checks remain required before serving.
Recovering or lagging candidates withdraw; expired registrations cannot campaign.
Duplicate node numbers fail registration. This is non-preemptive: a returning
lower-numbered node does not displace a healthy serving leader. The first ready
candidate may win at initial startup before other nodes become eligible.

Every new serving node should add a master candidate. Expand etcd explicitly via
learner/catch-up/promote, preserving member and cluster IDs. Existing client
builds do not automatically synchronize new endpoint lists; qualify and maintain
a stable local proxy endpoint before removing original members. Snapshot storage
must have independently qualified replica placement and availability.
