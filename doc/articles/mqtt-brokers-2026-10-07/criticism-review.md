# Review of the EMQX team's criticism

The comment arrived after the 2026-10-07 series was published. Some of it was true of the
snapshot it was written against and has since been fixed in the repository; some of it is true of
the current data as well; one part rests on a rerun that the article does not use. This note
records which is which, with the file that decides each point.

## Verdicts

### 1. The EMQX fan-in latency is a FIFO-pairing artifact - holds for the rerun

`LoadTest/results/raw/AWS-2026-09-28/record-EMQX-6.3.1-rerun.txt` is the run the comment quotes.
Its fan-in first interval received 2,693,200 of the 9,000,000 messages sent. The remaining
6,306,800, at the offered 50,000/s, are 126.136 s - and every later interval reports
126,136,02x us, varying by a few microseconds. That is the generator's FIFO queue of send times
catching up, not a measured broker latency.

The article's EMQX fan-in row is **not** that rerun: it is the earlier run in which EMQX reached
`force_shutdown.max_heap_size`, closed subscribers, and delivered 484,103 of 30,000,000. The
article says so. The rerun should nevertheless be discussed, because it shows both that EMQX can
sustain the offered rate once durable sessions are off and that the generator cannot report a
meaningful latency after a startup loss.

Action: a footnote in the article, and a scenario-engine change that either correlates by message
identity or reports the missing-message count next to the latency.

### 2. The persistence comparison is not the same test - does not hold for the 2026-10-07 series

The current series ran one scenario for every broker:
`LoadTest/Point-To-Point-60K-persistent.json`, 60,000 publishers and 60,000 subscribers,
`clean_session=false`, 60,000/s, ten minutes. `LoadTest/results/AWS/2026-10-07/Persistence.txt`
names that file in its header and shows every broker's run against it.

The mismatched 30K/60K-slowconnect comparison belongs to the earlier 0.9.18/0.9.19 data. It is
not used in the current article, and the old slowconnect files are kept only under
`LoadTest/results/AWS/0.9.19/` and `LoadTest/results/raw/AWS-2026-09-29/`.

Action: none in the article; make sure any future comparison uses one scenario file for all
brokers, as this one does.

### 3. The scenarios say MQTT 3 while the page says MQTT 5 - was true, fixed

The published snapshot had `protocol_version: 3` in several `LoadTest/` scenarios. Commit
`3d372d2` changed all five public scenarios to MQTT 5 on 2026-10-06, before the 10-07 series.
The scenarios under `test/ScenarioTests/` still use 3; they are the shipped examples and are not
the files the article links to.

Action: none for the article; the scenario files it names are MQTT 5.

### 4. The generator is the bottleneck by our own account - partially addressed

Three separate points:

- **Cost.** `CMakeLists.txt:55` says `xmq_scn` costs twice the broker per message and limits the
  heaviest scenarios. That was written for the scenario engine as it stands and is an admission,
  not a measurement of the client's CPU. The client's CPU was not recorded in these runs.
- **PaceMaker.** `common/PaceMaker.cpp` re-bases the schedule when the callers fall more than
  50 ms behind. The schedule is drift-free while it keeps up, but after a re-base the offered
  rate quietly drops; the article's rates are what the broker received, not necessarily what was
  asked for. This is coordinated omission.
- **Send timestamp.** The timestamp is taken at the actual send, not at the scheduled time, so
  a late send starts its latency measurement late as well.

Action: disclose this in the method section; add an offered-versus-delivered rate and a count of
PaceMaker re-bases to the scenario report. Recording client CPU is a larger change and belongs to
the next series.

### 5. Median is of interval means, and there are no percentiles - holds

`xmq_scn` keeps interval sums and counts. The `Median` line in a record is the median of the ten
interval averages; the article and the codex draft say exactly that. Per-message percentiles
cannot be recovered from the published files because the samples were never kept.

Action: keep the disclosure; implement a latency histogram in the scenario engine so that p50,
p95, p99 and p99.9 are reported per interval and per run. That is the only way a later article can
support a percentile claim.

### 6. Mosquitto and HiveMQ records, and the scenario parameters, are missing - was true, fixed

The records are now in `LoadTest/results/AWS/Mosquitto-2.1.2/` and
`LoadTest/results/AWS/HiveMQ-CE-2026.5/`, five scenarios each. The public scenario files now match
the published parameters: 100,000/s point-to-point, 5,000 connections/s, 5-minute fan-out, and
`LoadTest/500K-Connections-5000-rate.json` is in the repository.

Action: the article's links should point at those directories, not only at the site's charts.

### 7. One run, microsecond differences, no warm-up - holds as a limit of the data

The article already refuses to rank Mosquitto, FlashMQ and XMQ by a few microseconds in fan-in,
and calls the HiveMQ first interval warm-up a hypothesis. It does not claim statistical
significance. The runs are single runs, however, and the next series should use a fixed warm-up
and repeats for the key scenarios.

## What changed in the repository after the comment

- `3d372d2` - all public scenarios on MQTT 5, ten minutes (five for fan-out).
- `LoadTest/results/AWS/2026-10-07/` - one persistence scenario for every broker.
- `LoadTest/results/AWS/Mosquitto-2.1.2/`, `HiveMQ-CE-2026.5/` - the missing records.
- The article already discloses FIFO pairing, interval-median meaning, lack of percentiles, and
  the fact that a crash-loss window depends on Redis configuration.

## Work still to do

1. Add the generator-cost, PaceMaker and offered-versus-delivered disclosure to the article.
2. Add the EMQX rerun footnote: 50,000/s sustained after the first interval, constant 126.136 s
   because of 6,306,800 messages lost at startup, not a broker latency.
3. In `xmq_scn`: report the offered rate and the number of PaceMaker re-bases; add a per-message
   latency histogram and percentiles.
4. Then, and only then, consider message-identity correlation for fan-in. A sequence number in the
   payload changes the 16-byte payload the comparison uses, so it needs a deliberate decision and
   a new series, not a patch to the old one.
5. A new campaign with warm-up and repeats before any claim that separates brokers by microseconds.

## Status

Items 1-4 are implemented: both article drafts carry the generator-cost, PaceMaker and EMQX-rerun
notes; `xmq_scn` reports per-message percentiles and PaceMaker re-bases; a message's first four
payload bytes carry a 32-bit sequence, so a delivery is paired with its own send rather than with
whatever arrived next; and every run warms up for 30 seconds before measurement starts.

Item 5 remains in full: a warmed-up, repeated campaign over all brokers has not been run, so none
of the published numbers has changed. A 10,000/100 fan-in control on EMQX Enterprise 6.3.1 did
complete with the new generator: 2,999,997 deliveries at 10,000/s, median 353 us, p99 632 us, no
PaceMaker re-bases. The 50K/500 control is not reproducible on that instance: EMQX either kills
subscribers at its 48 MB heap limit or the host is OOM-killed at 512 MB.
