-- A call participant's robot: where it stands, how it moves, where its head looks, what its screen shows.
--
-- Attached to the root entity of each spawned robot level (behavior 'robot'). The host only says where
-- the participant is and puts the picture in a host texture; everything about the robot is decided here.
--
--   in  'robot_pose'  [id, x, y, z, yaw, pitch, speed]   (broadcast; assets/worlds/world.lua) where participant
--               `id` is: x/y/z the feet, yaw about +Y (0 faces +Z), pitch the look angle (positive looks up),
--               speed m/s (0: standing). Only the robot of that id listens.
--   in  'robot_screen'  [w, h]   (to this entity, every frame) the size of the picture in the screen's host texture
--
-- params (from the host):
--   id          the participant this robot stands for
--   prefix      the name prefix of the spawn: the robot's own entities are called "<prefix>/<name>"
--   screen      the name of the host texture shown on the "Screen" material slot
--   animations  the paths of the clips the model has, one per line

local BODY_RADIUS = 0.25 -- m: the capsule others bump into
local BODY_HEIGHT = 2.12 -- m: feet to the top of the head, matching the local player's capsule
local WALK_CLIP_SPEED = 0.8 -- m/s the Walk clip covers at speed 1
local HEAD_LIMIT = math.rad(45) -- the head nods this far up or down at most
local HEAD_STEP = math.rad(1) -- ... and is turned again only when the look angle moved this much
local SCREEN_ASPECT = 16 / 9

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- The path of the clip that contains `word`, from the newline-separated list.
local function find_clip(list, word)
  for path in string.gmatch(list or '', '[^\n]+') do
    if string.find(path, word, 1, true) then
      return path
    end
  end
  return nil
end

-- A picture of `w` x `h` fitted into the screen: the sampled rectangle (scale and offset in uv) that
-- shows all of it inside the 16:9 panel, centred; black bars outside.
local function fit(w, h)
  local aspect = w / h
  if aspect > SCREEN_ASPECT then
    -- wider than the panel: bars above and below
    local scale = aspect / SCREEN_ASPECT
    return 1, scale, 0, (1 - scale) * 0.5
  end
  local scale = SCREEN_ASPECT / aspect
  return scale, 1, (1 - scale) * 0.5, 0
end

behavior('robot', {
  on_create = function(self)
    local p = self.params
    local root = self.entity
    self.walking = nil -- unknown until the first pose: that one starts the clip
    self.clip_speed = 1
    self.tilt = 0
    self.idle = find_clip(p.animations, 'Idle')
    self.walk = find_clip(p.animations, 'Walk')

    -- A body for the others to bump into: kinematic, so it pushes nothing and falls nowhere; the pose
    -- below keeps it on the robot.
    component.add(root, 'rigid_body', { type = 1 })
    component.add(root, 'collider', {
      shape = 2,
      radius = BODY_RADIUS,
      half_height = (BODY_HEIGHT - 2 * BODY_RADIUS) * 0.5,
      offset = vec3.new(0, BODY_HEIGHT * 0.5, 0),
      friction = 0.5,
      density = 1000,
    })

    -- The model's own entities: the skinned mesh carries the Screen slot, the skeleton and the clips.
    self.body = entity.find(p.prefix .. '/Robot')
    local body = self.body
    if body then
      mesh.set_slot(body, 'Screen', 'albedo_tex', p.screen)
      mesh.set_slot(body, 'Screen', 'emissive_from_albedo', 1) -- a display lights itself, day or night
    end

    -- Out of sight until the first pose says where the participant is.
    transform.set_scale(root, vec3.new(0.001, 0.001, 0.001))

    -- The size of the picture in the screen's host texture: fit it into the 16:9 screen by sampling (black
    -- bars), when it changed.
    event.on('robot_screen', function(me, shape)
      if not body then return end
      local w, h = shape[1], shape[2]
      if w ~= me.shape_w or h ~= me.shape_h then
        local su, sv, ou, ov = fit(w, h)
        mesh.set_slot(body, 'Screen', 'albedo_uv', vec4.new(su, sv, ou, ov))
        mesh.set_slot(body, 'Screen', 'albedo_uv_outside', { 0, 0, 0, 1 }) -- opaque black outside
        me.shape_w, me.shape_h = w, h
      end
    end)

    event.on('robot_pose', function(me, at)
      if at[1] ~= p.id then return end
      if not me.shown then
        me.shown = true
        transform.set_scale(root, vec3.new(1, 1, 1))
      end
      local yaw = at[5]
      transform.set_position(root, vec3.new(at[2], at[3], at[4]))
      transform.set_rotation(root, quat.from_axis_angle(vec3.new(0, 1, 0), yaw))

      if not body then return end
      local speed = at[7]
      local walking = speed > 0
      local clip_speed = walking and speed / WALK_CLIP_SPEED or 1
      -- A call that fails (the skeleton is not loaded yet) leaves the state as it was: the next pose
      -- tries again.
      if walking ~= me.walking then
        local clip = walking and me.walk or me.idle
        if not clip or pcall(animation.play, body, clip, { speed = clip_speed }) then
          me.walking, me.clip_speed = walking, clip_speed
        end
      elseif walking and math.abs(clip_speed - me.clip_speed) > 0.01 then
        if pcall(animation.set_speed, body, clip_speed) then
          me.clip_speed = clip_speed
        end
      end

      -- The head follows where the participant looks: a nod about the joint's own X, set only when it
      -- moved by more than a step (the engine keeps it until it is set again). Looking up is a turn
      -- about -X.
      me.head = me.head or skeleton.find_joint(body, 'head')
      if me.head then
        local tilt = at[6]
        if tilt ~= tilt then tilt = 0 end -- NaN
        tilt = clamp(tilt, -HEAD_LIMIT, HEAD_LIMIT)
        if math.abs(tilt - me.tilt) > HEAD_STEP then
          animation.set_joint_rotation(body, me.head, quat.from_axis_angle(vec3.new(1, 0, 0), -tilt))
          me.tilt = tilt
        end
      end
    end)
  end,
})
