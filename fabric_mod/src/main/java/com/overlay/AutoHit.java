package com.overlay;

import net.minecraft.client.MinecraftClient;
import net.minecraft.client.network.ClientPlayerEntity;
import net.minecraft.entity.Entity;
import net.minecraft.entity.LivingEntity;
import net.minecraft.entity.effect.StatusEffects;
import net.minecraft.entity.mob.HostileEntity;
import net.minecraft.entity.passive.TameableEntity;
import net.minecraft.entity.player.PlayerEntity;
import net.minecraft.network.packet.c2s.play.ClientCommandC2SPacket;
import net.minecraft.util.Hand;
import net.minecraft.util.hit.EntityHitResult;
import net.minecraft.util.hit.HitResult;

import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;
import java.util.concurrent.ThreadLocalRandom;

/**
 * Автоудар (только одиночный мир). Настройки: %APPDATA%\MCOverlay\autohit.properties
 *
 * critOnly = true  : бьёт только критом (ждёт падения сколько нужно).
 * critOnly = false : ситуативный режим. На земле бьёт сразу; в воздухе ждёт
 *                    падения ради крита, но не дольше MAX_AIR_WAIT тиков.
 */
public final class AutoHit {

    /** true = писать в лог значения для отладки */
    private static final boolean DEBUG = false;

    /** Сколько тиков подряд надо падать, чтобы сервер засчитал крит */
    private static final int MIN_FALL_TICKS = 2;

    /** Максимум тиков ожидания крита в воздухе в ситуативном режиме */
    private static final int MAX_AIR_WAIT = 14;

    private record Settings(boolean enabled, double distance, boolean critOnly,
                            boolean players, boolean hostile, boolean passive,
                            boolean ignoreTamed, int delayMin, int delayMax) {
        static final Settings OFF = new Settings(false, 3.0, false, true, true, false, true, 0, 3);
    }

    private static final Path CONFIG_PATH = resolveConfigPath();
    private static volatile Settings settings = Settings.OFF;
    private static long lastCheckMs = 0;
    private static long lastModified = -1;
    private static long lastDebugMs = 0;
    private static int pendingDelay = -1;
    private static int fallTicks = 0; // тиков подряд движения вниз в воздухе
    private static int airWait = 0;   // сколько тиков ждём крит в воздухе

    private AutoHit() {}

    private static Path resolveConfigPath() {
        String base = System.getenv("APPDATA");
        if (base == null || base.isEmpty()) base = System.getProperty("user.home");
        return Path.of(base, "MCOverlay", "autohit.properties");
    }

    /** Вызывается каждый клиентский тик (из MinecraftClientMixin). */
    public static void onTick(MinecraftClient client) {
        try {
            updateFallTicks(client);
            reloadIfChanged();
            run(client);
        } catch (Throwable t) {
            reset();
            t.printStackTrace();
        }
    }

    private static void reset() {
        pendingDelay = -1;
        airWait = 0;
    }

    private static void updateFallTicks(MinecraftClient client) {
        ClientPlayerEntity p = client.player;
        if (p == null || p.isOnGround() || p.getVelocity().y >= 0.0
                || p.isTouchingWater() || p.isClimbing() || p.hasVehicle()) {
            fallTicks = 0;
        } else {
            fallTicks++;
        }
    }

    private static void run(MinecraftClient client) {
        Settings cfg = settings;
        ClientPlayerEntity player = client.player;
        if (!cfg.enabled() || player == null || client.world == null || client.interactionManager == null) {
            reset();
            return;
        }
        // Только одиночный мир
        if (!client.isInSingleplayer()) { reset(); return; }
        if (client.currentScreen != null || player.isSpectator() || player.isUsingItem()) { reset(); return; }

        // Свежая цель прицела
        client.gameRenderer.updateCrosshairTarget(1.0f);
        HitResult hit = client.crosshairTarget;
        if (!(hit instanceof EntityHitResult ehr)) { reset(); return; }

        Entity target = ehr.getEntity();
        if (!isValidTarget(cfg, player, target)) { reset(); return; }

        if (player.getEyePos().distanceTo(ehr.getPos()) > cfg.distance()) { reset(); return; }

        boolean canCritNow = canCrit(player);
        boolean airborne = isAirborne(player);

        // Перезарядка: в воздухе и в режиме критов достаточно > 0.9, на земле полная
        float cd = player.getAttackCooldownProgress(0.5f);
        float minCooldown = (cfg.critOnly() || airborne) ? 0.9f : 1.0f;
        if (cd < minCooldown) { pendingDelay = -1; return; }

        if (DEBUG) debug(player, cd, canCritNow, airborne);

        boolean crit = false;
        if (cfg.critOnly()) {
            // Строгий режим: только крит, ждём сколько нужно
            if (!canCritNow) return;
            crit = true;
        } else if (airborne) {
            // Ситуативный режим: в воздухе ждём падения, но не бесконечно
            if (canCritNow) {
                crit = true;
            } else if (++airWait < MAX_AIR_WAIT) {
                return;
            }
        }

        // Случайная задержка только для обычных ударов; критом бьём без неё
        if (pendingDelay < 0) {
            if (crit) {
                pendingDelay = 0;
            } else {
                int lo = Math.min(cfg.delayMin(), cfg.delayMax());
                int hi = Math.max(cfg.delayMin(), cfg.delayMax());
                pendingDelay = ThreadLocalRandom.current().nextInt(lo, hi + 1);
            }
        }
        if (pendingDelay > 0) { pendingDelay--; return; }

        client.interactionManager.attackEntity(player, target);
        player.swingHand(Hand.MAIN_HAND);
        reset();
    }

    private static boolean isValidTarget(Settings cfg, ClientPlayerEntity self, Entity e) {
        if (e == self || !(e instanceof LivingEntity living) || !living.isAlive()) return false;
        if (e instanceof PlayerEntity p) return cfg.players() && !p.isSpectator();
        if (e instanceof TameableEntity t && t.isTamed() && cfg.ignoreTamed()) return false;
        if (e instanceof HostileEntity) return cfg.hostile();
        return cfg.passive();
    }

    /** В воздухе, где крит вообще возможен (не вода, не лестница, не транспорт, не слепота). */
    private static boolean isAirborne(ClientPlayerEntity p) {
        return !p.isOnGround()
                && !p.isClimbing()
                && !p.isTouchingWater()
                && !p.hasVehicle()
                && !p.hasStatusEffect(StatusEffects.BLINDNESS);
    }

    /** Крит возможен: падаем минимум MIN_FALL_TICKS тиков, сервер уже накопил fallDistance. */
    private static boolean canCrit(ClientPlayerEntity p) {
        return fallTicks >= MIN_FALL_TICKS && isAirborne(p);
    }

    private static void debug(ClientPlayerEntity p, float cd, boolean canCrit, boolean airborne) {
        long now = System.currentTimeMillis();
        if (now - lastDebugMs < 200) return;
        lastDebugMs = now;
        System.out.println("[AutoHit] fallTicks=" + fallTicks
                + " airWait=" + airWait
                + " air=" + airborne
                + " canCrit=" + canCrit
                + " vy=" + p.getVelocity().y
                + " sprint=" + p.isSprinting()
                + " cd=" + cd);
    }

    // ---------- Чтение настроек ----------

    private static void reloadIfChanged() {
        long now = System.currentTimeMillis();
        if (now - lastCheckMs < 1000) return;
        lastCheckMs = now;
        try {
            if (!Files.exists(CONFIG_PATH)) { settings = Settings.OFF; lastModified = -1; return; }
            long mod = Files.getLastModifiedTime(CONFIG_PATH).toMillis();
            if (mod == lastModified) return;
            lastModified = mod;

            Properties p = new Properties();
            try (InputStream in = Files.newInputStream(CONFIG_PATH)) { p.load(in); }
            settings = new Settings(
                    bool(p, "enabled", false),
                    clamp(dbl(p, "distance", 3.0), 1.0, 6.0),
                    bool(p, "critOnly", false),
                    bool(p, "players", true),
                    bool(p, "hostile", true),
                    bool(p, "passive", false),
                    bool(p, "ignoreTamed", true),
                    (int) clamp(dbl(p, "delayMin", 0), 0, 10),
                    (int) clamp(dbl(p, "delayMax", 3), 0, 10));
        } catch (Exception ignored) {
            // оставляем прежние настройки
        }
    }

    private static boolean bool(Properties p, String k, boolean d) {
        String v = p.getProperty(k);
        return v == null ? d : v.trim().equals("1") || v.trim().equalsIgnoreCase("true");
    }

    private static double dbl(Properties p, String k, double d) {
        try { return Double.parseDouble(p.getProperty(k, String.valueOf(d)).trim()); }
        catch (NumberFormatException e) { return d; }
    }

    private static double clamp(double v, double lo, double hi) { return Math.max(lo, Math.min(hi, v)); }
}