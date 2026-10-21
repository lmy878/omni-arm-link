This directory holds generated WFB-NG keys.
Run scripts/gen_keys.sh ONCE and distribute the same pair to every node.

  drone.key  -> robot   (/etc/drone.key)
  gs.key     -> controller (/etc/gs.key)
  both       -> monitor (/etc/drone.key and /etc/gs.key)

drone.key and gs.key are two halves of one matched pair: they must come from
the same wfb_keygen run. Do not run gen_keys.sh separately on each host, or
the nodes will not be able to decrypt each other's packets.

Keys are gitignored.
