package com.overlay;

import net.minecraft.client.MinecraftClient;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Заменяет миксин: фоновый поток ставит работу в очередь клиентского потока.
 * Покадровая часть (PlayerNew, AimAssist) выполняется раз в кадр, AutoHit — раз в ~50 мс (клиентский тик).
 */
public final class AgentRuntime {
    private static final AtomicBoolean started = new AtomicBoolean(false);
    private static final AtomicBoolean pending = new AtomicBoolean(false);
    private static long lastTickMs = 0;

    private AgentRuntime() {}

    public static void start() {
        if (!started.compareAndSet(false, true)) return;
        new CoordsClientMod().onInitializeClient();   // канал с C++ оверлеем
        Visuals.start();                              // канал ESP / Nametag
        Thread t = new Thread(AgentRuntime::loop, "CoordsOverlay-Agent");
        t.setDaemon(true);
        t.start();
    }

    private static void loop() {
        while (true) {
            try {
                Thread.sleep(2);
                MinecraftClient mc = MinecraftClient.getInstance();
                if (mc == null || !pending.compareAndSet(false, true)) continue;
                mc.execute(() -> {
                    try { frame(mc); } finally { pending.set(false); }
                });
            } catch (InterruptedException e) {
                return;
            } catch (Throwable t) {
                pending.set(false);
            }
        }
    }

    private static void frame(MinecraftClient mc) {
        PlayerNew.onFrame(mc);
        AimAssist.onFrame(mc);
        Visuals.onFrame(mc);
        long now = System.currentTimeMillis();
        if (now - lastTickMs >= 50) {
            lastTickMs = now;
            AutoHit.onTick(mc);
        }
    }
}
