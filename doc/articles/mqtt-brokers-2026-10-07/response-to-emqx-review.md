# Response to the EMQX review

Thanks for reading the article and the records closely. Several of the points were right, and they
changed both the article and the generator. This is what we found, what changed, and what we still
cannot claim.

## The fan-in latency was a FIFO-pairing artifact

Confirmed. In `LoadTest/results/raw/AWS-2026-09-28/record-EMQX-6.3.1-rerun.txt` the fan-in first
interval received 2,693,200 of the 9,000,000 messages sent. The missing 6,306,800 at the offered
50,000/s are 126.136 s, and every later interval reports 126,136,02x us. The constant is the
generator's FIFO queue catching up with messages lost at startup, not a broker latency.

That rerun is not the EMQX row in the article: the article's row is the earlier run where EMQX
reached `force_shutdown.max_heap_size`, closed subscribers, and delivered 484,103 of 30,000,000.
The rerun is now described in both article drafts as the example of why FIFO pairing is unusable
after loss.

The generator no longer pairs by arrival order. The first four payload bytes carry a 32-bit
sequence number; a delivery is paired with its own send. A delivery with no matching send is
reported as unmatched instead of being paired with a stranger. Commit `7e090d0`.

## The generator's own limits are now measured and reported

- `PaceMaker` re-bases its schedule when callers fall 50 ms behind. Every run now reports how many
  times that happened; a non-zero count means the offered rate was lower for part of the run and
  the latency figures are optimistic.
- `xmq_scn` now reports per-message p50, p90, p95, p99, p99.9 (when there are at least a thousand
  samples) and max, not only interval means. The histogram has 64 buckets per octave, about 1.6%
  resolution. The old records kept only sums and counts, so their percentiles cannot be recovered.
- Every run warms up for 30 seconds by default, on the same clients, topics and rate; those
  samples and counts are discarded.
- Client CPU is not recorded yet. That is a next-series change, not something the old records can
  answer.

## What was true of the published snapshot and is fixed

- **MQTT 5.** The scenarios did carry `protocol_version: 3` in the snapshot the review looked at.
  Commit `3d372d2` changed all five public scenarios to MQTT 5 on 2026-10-06, before the 10-07
  series. The scenarios under `test/ScenarioTests/` still use 3; they are the shipped examples,
  not the files the article links to.
- **The missing Mosquitto and HiveMQ records.** They are in
  `LoadTest/results/AWS/Mosquitto-2.1.2/` and `LoadTest/results/AWS/HiveMQ-CE-2026.5/`, five
  scenarios each.
- **The scenario parameters.** The public files are the ones behind the published numbers:
  100,000/s point-to-point, 5,000 connections/s, five-minute fan-out.

## Where the criticism does not hold for the current series

- **The persistence comparison is the same test.** The 2026-10-07 series ran
  `LoadTest/Point-To-Point-60K-persistent.json` for every broker:
  60,000 publishers, 60,000 subscribers, `clean_session=false`, 60,000/s, ten minutes.
  `LoadTest/results/AWS/2026-10-07/Persistence.txt` names the file in its header. The 30K/60K
  slow-connect mismatch belongs to the older 0.9.18/0.9.19 data and is not used in the article.
- **Single runs and microseconds.** The article does not rank Mosquitto, FlashMQ and XMQ by a few
  microseconds in fan-in, and calls the HiveMQ first-interval result a hypothesis. The next series
  adds repeats and warm-up; until then we do not claim statistical significance.

## The 50K EMQX control

We tried to rerun the same 50K/500 fan-in against EMQX Enterprise 6.3.1 with the corrected
generator and a 30-second warm-up. It does not produce a usable control:

- at the default 48 MB heap limit, EMQX kills subscribers with `maximum heap size reached`
  (queues around 15,000 messages), and the measurement delivers nothing;
- at `force_shutdown.max_heap_size = 512MB`, the host is OOM-killed by systemd before the
  measurement ends.

A smaller 10,000/100 run on the same software and generator completed: 2,999,997 deliveries
at 10,000/s, median 353 us, p50 340 us, p99 632 us, p99.9 784 us, with no PaceMaker re-bases and
no heap kill during the measured window. It shows the new pairing and the 30-second warm-up
working; it does not pretend to be the 50K shape.

The 126-second artifact needs no rerun to be established: the arithmetic above uses the raw record
the review itself quoted. The smaller run shows the new pairing and warm-up working; the 50K shape
shows EMQX failing under the same limit the original run hit.

## What remains

A new full series with warm-up and repeats is planned before 1.0.1. None of the published numbers
has been changed by this review; the article and the method page now say what the old data can and
cannot support.

Files:

- `doc/articles/mqtt-brokers-2026-10-07/criticism-review.md` - point-by-point verdict with paths.
- `LoadTest/results/raw/AWS-2026-09-28/record-EMQX-6.3.1-rerun.txt` - the raw fan-in record.
- `LoadTest/results/AWS/2026-10-07/Persistence.txt` - the persistence series.
- Commits: `3d372d2`, `89669e1`, `7e090d0`, `1eaeebb`.
