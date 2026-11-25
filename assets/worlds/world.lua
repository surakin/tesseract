-- The 3D call room's shared state: who is where, what we tell the others, and how bodies push each other.
--
-- Attached to one entity by the host (behavior 'world'). The host only forwards bytes between the call's
-- data stream and this script; everything about the wire format and the rules is here.
--
--   in  'participants'  [local_id, id, id, ...]   everyone in the call, the local participant included
--   in  'frame'         [sender, bytes]           a frame that arrived from `sender`
--   in  'pose'          [x, z, yaw, pitch, speed, feet_y, eye_y]   the local player (assets/worlds/player.lua)
--   out 'send'          bytes                     a frame for everyone in the call
--   out 'robot_pose'    [id, x, y, z, yaw, pitch, speed]   where a remote participant is drawn (assets/worlds/robot.lua)
--   out 'reset'         [x, z, yaw, pitch]        the local player's spawn place (to the player script)
--   out 'nudge'         [dx, dz]                  the local player's share of an overlap (to the player script)
--
-- Wire format (docs/3D-DATA-STREAMS.md). Little endian.
--   pose frame, 32 bytes, kind 1:  version u8 (1), kind u8, flags u16 (bit 0: walking), seq u32, time_ms u32,
--     x f32, y f32, z f32 (the feet), yaw u16 (turns / 65536), pitch i16 (quarter turns / 32767), speed u16
--     (mm/s), reserved u16.
--   nudge frame, 21 + n bytes, kind 2: the same 12-byte header, dx f32, dz f32, n u8, then the target's id.
--
-- Remote poses are drawn RENDER_DELAY behind the newest frame, interpolated; the local pose goes out every
-- 50 ms while moving and every 500 ms standing. Bodies that overlap push each other apart, half and half,
-- each side moving only itself.

local TWO_PI = 2 * math.pi
local HALF_PI = math.pi / 2
local TWO32 = 4294967296

local HALF_EXTENT = 80 -- m: the meadow is 160 m square, centred on the origin
local MIN_HEIGHT, MAX_HEIGHT = -5, 50 -- m: a pose's feet height outside this is not believed
local MAX_PITCH = math.rad(80)
local SPAWN_RADIUS = 3 -- m

local RENDER_DELAY = 0.1 -- s
local STALE = 3 -- s: a participant silent this long is standing still
local WALKING_INTERVAL = 0.05 -- s
local STANDING_INTERVAL = 0.5 -- s
local INTERVAL_SLACK = 0.005 -- s: a timer that fires a little early must not skip a send and halve the rate
local YAW_STEP = TWO_PI / 65536 -- one quantisation step of the wire format
local PITCH_STEP = HALF_PI / 32767
local HEIGHT_STEP = 0.005 -- m: a change in height beyond this counts as moving (a jump, a slope)
local DRIFT_STEP = 0.01 -- m: sliding sideways beyond this counts as moving (a push)

local BODY_RADIUS = 0.25 -- m: the capsule of every player and robot
local BODY_HEIGHT = 2.12
local MAX_NUDGE = 0.25 -- m: a suggestion to move is believed up to this far ...
local NUDGE_LIFE = 0.25 -- s: ... and for this long
local NUDGE_INTERVAL = 0.1 -- s: between nudge frames to one participant
local NUDGE_REACH = 0.5 -- m: a push is believed from a player seen within this much more than touching
local NUDGE_PERIOD = 0.05 -- s: our own share of the overlaps is worked out this often

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function round(x)
  if x >= 0 then return math.floor(x + 0.5) end
  return -math.floor(-x + 0.5)
end

local function finite(x)
  return x == x and x ~= math.huge and x ~= -math.huge
end

-- An angle folded into [0, 2 pi).
local function wrap_turn(a)
  a = a % TWO_PI
  if a >= TWO_PI then return 0 end
  return a
end

-- Whether `a` is later than `b` in 32-bit sequence numbers, with wraparound.
local function seq_newer(a, b)
  local d = (a - b) % TWO32
  return d ~= 0 and d < TWO32 / 2
end

-- b - a of two 32-bit clocks, as a signed number.
local function clock_delta(b, a)
  local d = (b - a) % TWO32
  if d >= TWO32 / 2 then d = d - TWO32 end
  return d
end

-- Whether `data` is exactly `n` bytes long.
local function exactly(data, n)
  return pcall(bytes.unpack, data, 'B', n - 1) and not pcall(bytes.unpack, data, 'B', n)
end

-- Where a participant stands before they send anything: on a circle round the origin at an angle hashed from
-- their id (FNV-1a), facing the centre.
local function spawn_pose(id)
  local h = 2166136261
  for i = 1, #id do
    h = bit.bxor(h, string.byte(id, i)) % TWO32
    h = ((h % 256) * 16777216 + h * 403) % TWO32 -- h * 16777619 modulo 2^32, exact in a double
  end
  local angle = h / TWO32 * TWO_PI
  local sx, cz = math.sin(angle), math.cos(angle)
  return {
    x = SPAWN_RADIUS * sx, y = 0, z = SPAWN_RADIUS * cz,
    yaw = wrap_turn(math.atan2(-sx, -cz)), pitch = 0, speed = 0, walking = false,
  }
end

local function clamped(p)
  p.x = clamp(p.x, -HALF_EXTENT, HALF_EXTENT)
  p.z = clamp(p.z, -HALF_EXTENT, HALF_EXTENT)
  p.y = finite(p.y) and clamp(p.y, MIN_HEIGHT, MAX_HEIGHT) or 0
  return p
end

local function copy(p)
  return { x = p.x, y = p.y, z = p.z, yaw = p.yaw, pitch = p.pitch, speed = p.speed, walking = p.walking }
end

-- ---------------------------------------------------------------------------------------------------------
-- The wire format

local function encode_frame(seq, time_ms, p)
  local yaw = round(wrap_turn(p.yaw) / TWO_PI * 65536) % 65536
  local pitch = clamp(round(p.pitch / HALF_PI * 32767), -32767, 32767)
  local speed = clamp(round(p.speed * 1000), 0, 65535)
  return bytes.pack('B B H I I f f f H h H H', 1, 1, p.walking and 1 or 0, seq, time_ms, p.x, p.y, p.z, yaw, pitch,
    speed, 0)
end

-- A pose frame's seq, time_ms and pose, or nil when the data is not one.
local function decode_frame(data)
  if not exactly(data, 32) then return nil end
  local version, kind, flags, seq, time_ms, x, y, z, yaw, pitch, speed = bytes.unpack(data, 'B B H I I f f f H h H')
  if version ~= 1 or kind ~= 1 or not (finite(x) and finite(y) and finite(z)) then return nil end
  return seq, time_ms, {
    x = x, y = y, z = z,
    yaw = yaw * TWO_PI / 65536, pitch = pitch / 32767 * HALF_PI, speed = speed / 1000,
    walking = flags % 2 == 1,
  }
end

local function encode_nudge(seq, time_ms, target, dx, dz)
  if #target == 0 or #target > 255 or not (finite(dx) and finite(dz)) then return nil end
  return bytes.pack('B B H I I f f B c' .. #target, 1, 2, 0, seq, time_ms, dx, dz, #target, target)
end

-- A nudge frame's seq, target and (dx, dz), or nil when the data is not one.
local function decode_nudge(data)
  local ok, version, kind, _, seq, _, dx, dz, n = pcall(bytes.unpack, data, 'B B H I I f f B')
  if not ok or version ~= 1 or kind ~= 2 or n == 0 or not (finite(dx) and finite(dz)) then return nil end
  if not exactly(data, 21 + n) then return nil end
  return seq, bytes.unpack(data, 'c' .. n, 21), dx, dz
end

-- ---------------------------------------------------------------------------------------------------------
-- Bodies

-- Two bodies at feet positions a and b that overlap are pushed apart. Returns the horizontal displacement that b
-- should move by to take half of the overlap (a takes the other half, the other way), or nil when they do not
-- overlap. `a_first` breaks the tie when the axes coincide and is true for exactly one of the two sides.
local function collision_push(a, b, a_first)
  if math.abs(a.y - b.y) >= BODY_HEIGHT then return nil end
  local ox, oz = b.x - a.x, b.z - a.z
  local dist = math.sqrt(ox * ox + oz * oz)
  local depth = 2 * BODY_RADIUS - dist
  if not (depth > 0) then return nil end
  local nx, nz
  if dist > 1e-4 then
    nx, nz = ox / dist, oz / dist
  else
    nx, nz = a_first and 1 or -1, 0
  end
  return nx * depth * 0.5, nz * depth * 0.5
end

-- ---------------------------------------------------------------------------------------------------------

local on_pose_frame, sample, on_nudge, due_frame, due_nudges, take_nudge

-- A pose frame of `sender`.
function on_pose_frame(self, sender, data, now)
  local seq, time32, pose = decode_frame(data)
  if not seq then return end
  clamped(pose)
  local r = self.remotes[sender]
  if not r then
    self.remotes[sender] = {
      newest = pose, newest_ms = time32, previous = nil, previous_ms = 0,
      seq = seq, time32 = time32, offset = now - time32 / 1000, arrival = now,
    }
    return
  end
  if not seq_newer(seq, r.seq) then return end
  local ms = r.newest_ms + clock_delta(time32, r.time32)
  r.previous, r.previous_ms = r.newest, r.newest_ms
  r.newest, r.newest_ms = pose, ms
  r.seq, r.time32 = seq, time32
  r.offset = math.min(r.offset, now - ms / 1000)
  r.arrival = now
end

-- Where `id` is drawn at `now`: interpolated, a little behind the newest frame. nil when nothing arrived.
function sample(self, id, now)
  local r = self.remotes[id]
  if not r then return nil end
  if now - r.arrival > STALE then
    local p = copy(r.newest)
    p.speed, p.walking = 0, false
    return p
  end
  local target = now - RENDER_DELAY
  local t1 = r.newest_ms / 1000 + r.offset
  if not r.previous or target >= t1 then return r.newest end
  local a, b = r.previous, r.newest
  local t0 = r.previous_ms / 1000 + r.offset
  if target <= t0 or t1 <= t0 then return a end
  local f = (target - t0) / (t1 - t0)
  local p = copy(a) -- the walking state of the earlier frame
  p.x = a.x + (b.x - a.x) * f
  p.y = a.y + (b.y - a.y) * f
  p.z = a.z + (b.z - a.z) * f
  p.pitch = a.pitch + (b.pitch - a.pitch) * f
  p.speed = a.speed + (b.speed - a.speed) * f
  local d = (b.yaw - a.yaw) % TWO_PI -- the shortest way round
  if d > math.pi then d = d - TWO_PI end
  p.yaw = wrap_turn(a.yaw + d * f)
  return p
end

-- Another player's request to move us. Believed only from a player who is close to us in our own view, only
-- away from them, by no more than our own half of that overlap would be (with some slack for the lag).
function on_nudge(self, sender, data, now)
  local seq, target, dx, dz = decode_nudge(data)
  if not seq or target ~= self.local_id then return end
  local seen = self.nudge_seq[sender]
  if seen and not seq_newer(seq, seen) then return end
  self.nudge_seq[sender] = seq
  local accepted = self.nudge_accept[sender]
  if accepted and now - accepted < NUDGE_INTERVAL - 0.01 then return end -- one from each player per interval
  local there = sample(self, sender, now)
  local mine = self.me
  if not there or not mine or math.abs(there.y - mine.y) >= BODY_HEIGHT then return end
  local ox, oz = mine.x - there.x, mine.z - there.z
  local dist = math.sqrt(ox * ox + oz * oz)
  local depth = 2 * BODY_RADIUS + NUDGE_REACH - dist
  if not (depth > 0) then return end
  local ax, az = ox / math.max(dist, 1e-4), oz / math.max(dist, 1e-4)
  if dist <= 1e-4 then
    ax, az = (sender < self.local_id) and 1 or -1, 0 -- the tie rule of collision_push
  end
  local use = clamp(dx * ax + dz * az, 0, math.min(depth * 0.5, MAX_NUDGE))
  if not (use > 0) then return end
  self.nudge_accept[sender] = now
  self.suggestions['from:' .. sender] = { dx = ax * use, dz = az * use, at = now }
end

-- The pose frame to send now, or nil when nothing is due.
function due_frame(self, now)
  local p = self.me
  local last = self.last_sent_pose
  -- Turning or looking is moving too: the pose changed by more than a wire step since the last frame.
  local turning, rising, sliding = false, false, false
  if last then
    local dyaw = (p.yaw - last.yaw) % TWO_PI
    if dyaw > math.pi then dyaw = dyaw - TWO_PI end
    turning = math.abs(dyaw) > YAW_STEP or math.abs(p.pitch - last.pitch) > PITCH_STEP
    rising = math.abs(p.y - last.y) > HEIGHT_STEP
    sliding = math.sqrt((p.x - last.x) ^ 2 + (p.z - last.z) ^ 2) > DRIFT_STEP
  end
  local moving = p.walking or turning or rising or sliding
  local interval = moving and WALKING_INTERVAL or STANDING_INTERVAL
  local stopped = self.last_sent_moving and not moving -- tell the others at once when walking or turning stops
  if self.last_sent and not stopped and now - self.last_sent < interval - INTERVAL_SLACK then return nil end
  self.last_sent, self.last_sent_moving, self.last_sent_pose = now, moving, copy(p)
  local seq = self.next_seq
  self.next_seq = (seq + 1) % TWO32
  return encode_frame(seq, math.floor(now * 1000) % TWO32, p)
end

-- Players that overlap push each other apart, half and half: the overlap with every robot we know is worked
-- out from the local pose and the robots as drawn. Returns the nudge frames to send, one for each overlapped
-- participant at most every NUDGE_INTERVAL; our own half of each push is queued for the local player.
function due_nudges(self, now)
  local frames = {}
  for _, id in ipairs(self.order) do
    local there = sample(self, id, now)
    local dx, dz
    if there then dx, dz = collision_push(self.me, there, self.local_id < id) end
    if not dx then
      self.suggestions['own:' .. id] = nil
    else
      -- We move half the overlap away from them; they are asked to move the other half.
      self.suggestions['own:' .. id] = { dx = -dx, dz = -dz, at = now }
      local sent = self.nudge_sent[id]
      if not sent or now - sent >= NUDGE_INTERVAL then
        self.nudge_sent[id] = now
        local seq = self.next_seq
        self.next_seq = (seq + 1) % TWO32
        local frame = encode_nudge(seq, math.floor(now * 1000) % TWO32, id, dx, dz)
        if frame then frames[#frames + 1] = frame end
      end
    end
  end
  return frames
end

-- How far the local player should move now: the mean of the queued suggestions, ours and the ones other
-- players sent about us lately. Each is used once.
function take_nudge(self, now)
  local sx, sz, count = 0, 0, 0
  for _, s in pairs(self.suggestions) do
    if now - s.at <= NUDGE_LIFE then
      sx, sz, count = sx + s.dx, sz + s.dz, count + 1
    end
  end
  self.suggestions = {}
  if count == 0 then return nil end
  return sx / count, sz / count
end


behavior('world', {
  on_create = function(self)
    self.local_id = nil
    self.present = {} -- id -> true, everyone else in the call
    self.order = {} -- the same ids, sorted: a stable iteration order
    self.remotes = {} -- id -> { newest, previous, seq, time32, offset, arrival }
    self.me = nil -- the local pose
    self.next_seq = 0
    self.last_sent = nil -- when the last pose frame went out
    self.last_sent_moving = false
    self.last_sent_pose = nil
    self.nudge_sent = {} -- when a nudge frame last went to each participant
    self.nudge_seq = {} -- the newest nudge frame received from each
    self.nudge_accept = {} -- when one from each was last believed
    self.suggestions = {} -- "own:<id>" / "from:<id>" -> { dx, dz, at }
    self.last_nudge_step = -1
    self.pending_reset = false

    event.on('participants', function(me, list)
      local id = list[1]
      if type(id) ~= 'string' then return end
      if id ~= me.local_id then
        me.local_id = id
        me.me = spawn_pose(id)
        me.remotes, me.nudge_sent, me.nudge_seq, me.nudge_accept, me.suggestions = {}, {}, {}, {}, {}
        me.next_seq, me.last_sent, me.last_sent_moving, me.last_sent_pose = 0, nil, false, nil
        me.pending_reset = true
      end
      local present, order = {}, {}
      for i = 2, #list do
        local other = list[i]
        if type(other) == 'string' and other ~= id and not present[other] then
          present[other] = true
          order[#order + 1] = other
        end
      end
      table.sort(order)
      -- Forget the ones who left.
      for other in pairs(me.remotes) do
        if not present[other] then me.remotes[other] = nil end
      end
      for other in pairs(me.nudge_sent) do
        if not present[other] then me.nudge_sent[other] = nil end
      end
      for other in pairs(me.nudge_seq) do
        if not present[other] then me.nudge_seq[other] = nil end
      end
      for other in pairs(me.nudge_accept) do
        if not present[other] then me.nudge_accept[other] = nil end
      end
      for key in pairs(me.suggestions) do
        if not present[string.sub(key, string.find(key, ':', 1, true) + 1)] then me.suggestions[key] = nil end
      end
      me.present, me.order = present, order
    end)

    event.on('pose', function(me, v)
      if not me.local_id then return end
      local p = clamped({ x = v[1], y = v[6], z = v[2], yaw = v[3], pitch = v[4], speed = v[5] })
      p.yaw = wrap_turn(p.yaw)
      p.pitch = clamp(p.pitch, -MAX_PITCH, MAX_PITCH)
      p.walking = v[5] > 0
      me.me = p
    end)

    event.on('frame', function(me, v)
      local sender, data = v[1], v[2]
      if not me.local_id or sender == me.local_id or type(sender) ~= 'string' then return end
      local ok, version, kind = pcall(bytes.unpack, data, 'B B')
      if not ok or version ~= 1 then return end
      local now = time.real()
      if kind == 2 then
        on_nudge(me, sender, data, now)
      elseif kind == 1 then
        on_pose_frame(me, sender, data, now)
      end
    end)
  end,

  on_update = function(self)
    if not self.local_id then return end
    local now = time.real()
    if self.pending_reset then
      self.pending_reset = false
      local s = self.me
      event.emit('reset', { s.x, s.z, s.yaw, s.pitch })
    end
    -- Where everyone else is drawn.
    for _, id in ipairs(self.order) do
      local p = sample(self, id, now) or spawn_pose(id)
      event.emit('robot_pose', { id, p.x, p.y, p.z, p.yaw, p.pitch, p.walking and p.speed or 0 })
    end
    local frame = due_frame(self, now)
    if frame then event.emit('send', frame) end
    if now - self.last_nudge_step >= NUDGE_PERIOD - INTERVAL_SLACK then
      self.last_nudge_step = now
      for _, f in ipairs(due_nudges(self, now)) do
        event.emit('send', f)
      end
    end
    local dx, dz = take_nudge(self, now)
    if dx then event.emit('nudge', { dx, dz }) end
  end,
})
