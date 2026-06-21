import 'dart:convert';
import 'package:http/http.dart' as http;

class Esp32Service {
  static const _base = 'http://172.20.10.2';

  static Future<void> startSession({
    required String name,
    required String userId,
  }) async {
    await http.post(
      Uri.parse('$_base/session/start'),
      headers: {'Content-Type': 'application/json'},
      body: jsonEncode({'name': name, 'userId': userId}),
    ).timeout(const Duration(seconds: 5));
  }

  static Future<void> endSession({
    required String userId,
    required int bottles,
  }) async {
    await http.post(
      Uri.parse('$_base/session/end'),
      headers: {'Content-Type': 'application/json'},
      body: jsonEncode({'userId': userId, 'bottles': bottles}),
    ).timeout(const Duration(seconds: 5));
  }
}
