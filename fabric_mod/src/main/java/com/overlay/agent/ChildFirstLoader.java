package com.overlay.agent;

import java.net.URL;
import java.net.URLClassLoader;

/** Классы com.overlay.* (кроме agent) берёт из jar сам, остальное (net.minecraft.*) — у загрузчика игры. */
final class ChildFirstLoader extends URLClassLoader {
    ChildFirstLoader(URL[] urls, ClassLoader parent) { super(urls, parent); }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        synchronized (getClassLoadingLock(name)) {
            if (name.startsWith("com.overlay.") && !name.startsWith("com.overlay.agent.")) {
                Class<?> c = findLoadedClass(name);
                if (c == null) c = findClass(name);
                if (resolve) resolveClass(c);
                return c;
            }
            return super.loadClass(name, resolve);
        }
    }
}
