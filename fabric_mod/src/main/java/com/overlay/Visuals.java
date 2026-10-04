package com.overlay;

import net.minecraft.client.MinecraftClient;
import net.minecraft.client.network.ClientPlayerEntity;
import net.minecraft.client.render.Camera;
import net.minecraft.client.world.ClientWorld;
import net.minecraft.entity.Entity;
import net.minecraft.entity.EquipmentSlot;
import net.minecraft.entity.LivingEntity;
import net.minecraft.entity.mob.MobEntity;
import net.minecraft.entity.player.PlayerEntity;
import net.minecraft.item.ItemStack;
import net.minecraft.registry.Registries;
import net.minecraft.util.Arm;
import net.minecraft.util.math.MathHelper;
import net.minecraft.util.math.Vec3d;

import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * ESP + Nametag (ТОЛЬКО одиночный мир, проверка isInSingleplayer).
 *
 * Мод НЕ считает проекцию на экран. Каждый кадр он отправляет в C++ оверлей
 * (канал \\.\pipe\mc_esp) мировые координаты камеры и существ вместе со скоростями.
 * Оверлей сам проецирует их на каждый свой кадр с экстраполяцией по скорости,
 * поэтому рамки двигаются плавно, а не "скачут" по частоте игровых кадров.
 *
 * Пакет (little-endian):
 *   int32 magic=0x3253454D, int32 count,
 *   double camX,camY,camZ; float yaw,pitch,fov; float camVx,camVy,camVz (блоков/с), camVyaw,camVpitch (град/с),
 *   count раз:
 *     double x,y,z (ноги); float vx,vy,vz; float width,height; float hp,maxHp,absorb; int32 isPlayer; str name;
 *     6 слотов (правая рука, левая рука, шлем, нагрудник, поножи, ботинки):
 *       int32 durability%(-1 пусто), int32 count, str itemId, str itemName
 *   str = uint16 длина + UTF-16LE.
 *
 * Совет: в настройках игры "Эффекты поля зрения" = 0% и "Покачивание камеры" выключить.
 */
public final class Visuals {

    private static final String PIPE_PATH = "\\\\.\\pipe\\mc_esp";
    private static final int MAGIC = 0x3253454D;
    private static final int MAX_ENTITIES = 40;
    private static final int MAX_STR = 40;

    private record Settings(boolean esp, boolean nametag, boolean players, boolean mobs, double range) {
        static final Settings OFF = new Settings(false, false, true, false, 64.0);
        boolean active() { return esp || nametag; }
    }

    private static final Path CONFIG_PATH = resolveConfigPath();
    private static volatile Settings settings = Settings.OFF;
    private static long lastCheckMs = 0;
    private static long lastModified = -1;

    private static final ByteBuffer BUF = ByteBuffer.allocate(64 * 1024).order(ByteOrder.LITTLE_ENDIAN);

    // Передача в поток записи
    private static final AtomicBoolean started = new AtomicBoolean(false);
    private static final Object LOCK = new Object();
    private static byte[] latest = null;
    private static boolean fresh = false;
    private static boolean lastWasEmpty = false;

    // Для расчёта скоростей
    private static long prevNs = 0;
    private static double pCamX, pCamY, pCamZ;
    private static float pYaw, pPitch;
    private static float vcx, vcy, vcz, vyaw, vpitch;
    private static Map<Integer, double[]> prevEnt = new HashMap<>();   // id -> x,y,z,vx,vy,vz

    private Visuals() {}

    private static Path resolveConfigPath() {
        String base = System.getenv("APPDATA");
        if (base == null || base.isEmpty()) base = System.getProperty("user.home");
        return Path.of(base, "MCOverlay", "visuals.properties");
    }

    // ---------- Поток отправки (просыпается сразу при новом кадре) ----------

    public static void start() {
        if (!started.compareAndSet(false, true)) return;
        Thread t = new Thread(Visuals::writerLoop, "CoordsOverlay-EspPipe");
        t.setDaemon(true);
        t.start();
    }

    private static void writerLoop() {
        FileOutputStream pipe = null;
        while (true) {
            try {
                byte[] data;
                synchronized (LOCK) {
                    while (!fresh) LOCK.wait();
                    data = latest;
                    fresh = false;
                }
                if (pipe == null) {
                    try {
                        pipe = new FileOutputStream(PIPE_PATH);
                    } catch (IOException e) {
                        Thread.sleep(400);   // оверлей ещё не запущен
                        continue;
                    }
                }
                pipe.write(data);
                pipe.flush();
            } catch (IOException e) {
                try { if (pipe != null) pipe.close(); } catch (IOException ignored) {}
                pipe = null;
            } catch (InterruptedException e) {
                return;
            } catch (Throwable ignored) {
            }
        }
    }

    private static void publish(byte[] data, boolean empty) {
        synchronized (LOCK) {
            lastWasEmpty = empty;
            latest = data;
            fresh = true;
            LOCK.notifyAll();
        }
    }

    // ---------- Настройки ----------

    private static void reloadIfChanged() {
        long now = System.currentTimeMillis();
        if (now - lastCheckMs < 500) return;
        lastCheckMs = now;
        try {
            if (!Files.exists(CONFIG_PATH)) { settings = Settings.OFF; lastModified = -1; return; }
            long m = Files.getLastModifiedTime(CONFIG_PATH).toMillis();
            if (m == lastModified) return;
            lastModified = m;
            Properties p = new Properties();
            try (InputStream in = Files.newInputStream(CONFIG_PATH)) { p.load(in); }
            double range = 64.0;
            try { range = Double.parseDouble(p.getProperty("range", "64").trim()); } catch (NumberFormatException ignored) {}
            range = Math.max(8.0, Math.min(128.0, range));
            settings = new Settings(
                    flag(p, "esp", false), flag(p, "nametag", false),
                    flag(p, "players", true), flag(p, "mobs", false), range);
        } catch (Throwable t) {
            settings = Settings.OFF;
        }
    }

    private static boolean flag(Properties p, String k, boolean def) {
        String v = p.getProperty(k);
        if (v == null) return def;
        v = v.trim();
        return v.equals("1") || v.equalsIgnoreCase("true");
    }

    // ---------- Основной цикл ----------

    /** Вызывается каждый кадр из AgentRuntime. */
    public static void onFrame(MinecraftClient mc) {
        try {
            update(mc);
        } catch (Throwable t) {
            resetState();
            publishEmpty();
        }
    }

    private static void resetState() {
        prevNs = 0;
        prevEnt = new HashMap<>();
    }

    private static void update(MinecraftClient mc) {
        reloadIfChanged();
        Settings s = settings;
        ClientPlayerEntity self = mc.player;
        ClientWorld world = mc.world;

        // GATE: только одиночный мир
        if (!s.active() || self == null || world == null || !mc.isInSingleplayer()) {
            resetState();
            publishEmpty();
            return;
        }

        float td = mc.getRenderTickCounter().getTickDelta(false);

        // Камера. От первого лица берём данные игрока напрямую: они свежее, чем у Camera
        // (Camera обновляется только во время отрисовки кадра).
        double camX, camY, camZ;
        float yaw, pitch;
        if (mc.options.getPerspective().isFirstPerson()) {
            Vec3d eye = self.getCameraPosVec(td);
            camX = eye.x; camY = eye.y; camZ = eye.z;
            yaw = self.getYaw(td);
            pitch = self.getPitch(td);
        } else {
            Camera cam = mc.gameRenderer.getCamera();
            Vec3d p = cam.getPos();
            camX = p.x; camY = p.y; camZ = p.z;
            yaw = cam.getYaw();
            pitch = cam.getPitch();
        }
        float fov = (float) (double) mc.options.getFov().getValue();

        // Скорости камеры (по разнице между кадрами)
        long nowNs = System.nanoTime();
        double dt = prevNs == 0 ? 0.0 : (nowNs - prevNs) / 1e9;
        if (dt > 0.25) { dt = 0.0; prevNs = 0; prevEnt = new HashMap<>(); }
        boolean usable = dt >= 0.004;           // слишком короткие интервалы дают шум
        if (usable) {
            vcx = lerpV(vcx, (float) ((camX - pCamX) / dt));
            vcy = lerpV(vcy, (float) ((camY - pCamY) / dt));
            vcz = lerpV(vcz, (float) ((camZ - pCamZ) / dt));
            vyaw = lerpV(vyaw, (float) (MathHelper.wrapDegrees(yaw - pYaw) / dt));
            vpitch = lerpV(vpitch, (float) ((pitch - pPitch) / dt));
        } else if (prevNs == 0 || dt == 0.0) {
            vcx = vcy = vcz = vyaw = vpitch = 0f;
        }
        if (usable || prevNs == 0 || dt == 0.0) {
            pCamX = camX; pCamY = camY; pCamZ = camZ; pYaw = yaw; pPitch = pitch;
        }
        // ограничение скорости поворота: резкие рывки мыши не должны давать огромную экстраполяцию
        float vyawC = MathHelper.clamp(vyaw, -900f, 900f);
        float vpitchC = MathHelper.clamp(vpitch, -900f, 900f);

        double r2 = s.range() * s.range();
        Map<Integer, double[]> nextEnt = new HashMap<>();

        // Собираем кандидатов
        java.util.List<LivingEntity> list = new java.util.ArrayList<>();
        for (Entity e : world.getEntities()) {
            if (!(e instanceof LivingEntity le) || e == self || !le.isAlive()) continue;
            boolean isPlayer = le instanceof PlayerEntity;
            if (isPlayer) {
                if (!s.players() || ((PlayerEntity) le).isSpectator()) continue;
            } else if (!(s.mobs() && le instanceof MobEntity)) {
                continue;
            }
            if (self.squaredDistanceTo(le) > r2) continue;
            list.add(le);
        }
        if (list.size() > MAX_ENTITIES) {
            list.sort((a, b) -> Double.compare(self.squaredDistanceTo(a), self.squaredDistanceTo(b)));
            list = list.subList(0, MAX_ENTITIES);
        }
        if (list.isEmpty()) {
            prevNs = nowNs; prevEnt = nextEnt;
            publishEmpty();
            return;
        }

        BUF.clear();
        BUF.putInt(MAGIC);
        BUF.putInt(list.size());
        BUF.putDouble(camX).putDouble(camY).putDouble(camZ);
        BUF.putFloat(yaw).putFloat(pitch).putFloat(fov);
        BUF.putFloat(vcx).putFloat(vcy).putFloat(vcz).putFloat(vyawC).putFloat(vpitchC);

        for (LivingEntity le : list) {
            Vec3d pos = le.getLerpedPos(td);
            float vx = 0, vy = 0, vz = 0;
            double[] pe = prevEnt.get(le.getId());
            if (pe != null && usable) {
                vx = lerpV((float) pe[3], (float) ((pos.x - pe[0]) / dt));
                vy = lerpV((float) pe[4], (float) ((pos.y - pe[1]) / dt));
                vz = lerpV((float) pe[5], (float) ((pos.z - pe[2]) / dt));
            } else if (pe != null) {
                vx = (float) pe[3]; vy = (float) pe[4]; vz = (float) pe[5];
            }
            // обновляем историю, только если интервал нормальный (иначе храним прошлую позицию)
            if (pe == null || usable) nextEnt.put(le.getId(), new double[]{pos.x, pos.y, pos.z, vx, vy, vz});
            else nextEnt.put(le.getId(), pe);

            BUF.putDouble(pos.x).putDouble(pos.y).putDouble(pos.z);
            BUF.putFloat(vx).putFloat(vy).putFloat(vz);
            BUF.putFloat(le.getWidth()).putFloat(le.getHeight());
            BUF.putFloat(le.getHealth()).putFloat(le.getMaxHealth()).putFloat(le.getAbsorptionAmount());
            BUF.putInt(le instanceof PlayerEntity ? 1 : 0);
            putString(le.getName().getString());

            boolean rightMain = le.getMainArm() == Arm.RIGHT;
            ItemStack main = le.getMainHandStack(), off = le.getOffHandStack();
            putSlot(rightMain ? main : off);                       // правая рука
            putSlot(rightMain ? off : main);                       // левая рука
            putSlot(le.getEquippedStack(EquipmentSlot.HEAD));
            putSlot(le.getEquippedStack(EquipmentSlot.CHEST));
            putSlot(le.getEquippedStack(EquipmentSlot.LEGS));
            putSlot(le.getEquippedStack(EquipmentSlot.FEET));
        }
        if (usable || prevNs == 0) prevNs = nowNs;
        prevEnt = nextEnt;
        publish(Arrays.copyOf(BUF.array(), BUF.position()), false);
    }

    // ---------- Вспомогательное ----------

    /** Лёгкое сглаживание скорости, чтобы не было шума от неравномерных кадров. */
    private static float lerpV(float old, float cur) {
        return old + (cur - old) * 0.6f;
    }

    private static void putSlot(ItemStack st) {
        if (st == null || st.isEmpty()) {
            BUF.putInt(-1).putInt(0);
            putString("");
            putString("");
            return;
        }
        int dur = 100;
        if (st.isDamageable() && st.getMaxDamage() > 0) {
            dur = Math.max(0, Math.min(100, Math.round(100.0f * (st.getMaxDamage() - st.getDamage()) / st.getMaxDamage())));
        }
        BUF.putInt(dur).putInt(st.getCount());
        putString(Registries.ITEM.getId(st.getItem()).getPath());
        putString(st.getName().getString());
    }

    private static void putString(String s) {
        if (s == null) s = "";
        if (s.length() > MAX_STR) s = s.substring(0, MAX_STR - 1) + "\u2026";
        BUF.putShort((short) s.length());
        for (int i = 0; i < s.length(); i++) BUF.putChar(s.charAt(i));   // LE -> UTF-16LE
    }

    private static void publishEmpty() {
        if (lastWasEmpty) return;
        ByteBuffer b = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(MAGIC).putInt(0);
        // пустой пакет: заголовок камеры не нужен, оверлей отбросит его и очистит список
        publish(b.array(), true);
    }
}
