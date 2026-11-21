Splash fix kaise hua — ek line
File: MundoCore/src/main/java/com/mundo/app/configuration/ClientConfiguration.java
// BEFORE — splash ON
public boolean isEnableLauncherActivity() {
    return true;
}

// AFTER — splash OFF ✅
public boolean isEnableLauncherActivity() {
    return false;
}
Kya hota hai andar:
bbox framework mein ek central config class hai — ClientConfiguration. Jab bhi koi guest app launch hota hai, framework pehle isEnableLauncherActivity() check karta hai:
true → LauncherActivity.launch() call hota hai → white screen + icon + loading bar dikhta hai, fir game load
false → LauncherActivity completely skip, directly getBActivityManager().startActivity() call hota hai → zero splash, instant launch
Matlab LauncherActivity.java ko touch karne ki zarurat hi nahi thi. Chahe usme kitna bhi code likh do — jab tak ClientConfiguration mein true tha, splash dikhta rehega. Root switch wahan tha, file mein nahi.