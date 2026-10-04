package com.overlay.agent;

import java.io.File;
import java.lang.instrument.Instrumentation;
import java.net.URL;

/**
 * Точка входа Java-агента (подключается через Attach API уже после запуска игры).
 * Находит загрузчик классов Minecraft/Fabric (Knot), создаёт поверх него загрузчик
 * "child-first" из этого же jar и запускает com.overlay.AgentRuntime.
 * Jar должен быть собран через remapJar (имена intermediary, как в рантайме Fabric).
 */
public final class AgentMain {
    private AgentMain() {}

    public static void agentmain(String args, Instrumentation inst) { run(inst); }
    public static void premain(String args, Instrumentation inst) { run(inst); }

    private static void run(Instrumentation inst) {
        try {
            ClassLoader game = null;
            for (Class<?> c : inst.getAllLoadedClasses()) {
                if (c.getName().equals("net.minecraft.class_310")) { game = c.getClassLoader(); break; }
            }
            if (game == null) {
                System.err.println("[CoordsOverlay] Класс MinecraftClient (class_310) не найден: нужен Fabric 1.21.4 (production).");
                return;
            }
            File jar = new File(AgentMain.class.getProtectionDomain().getCodeSource().getLocation().toURI());
            ChildFirstLoader cl = new ChildFirstLoader(new URL[]{ jar.toURI().toURL() }, game);
            cl.loadClass("com.overlay.AgentRuntime").getMethod("start").invoke(null);
            System.out.println("[CoordsOverlay] Agent started");
        } catch (Throwable t) {
            t.printStackTrace();
        }
    }
}
