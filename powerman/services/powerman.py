#!/usr/bin/env python
"""
  ___________________________________________________
 |  _____                       _____ _ _       _    |
 | |  __ \                     |  __ (_) |     | |   |
 | | |__) |__ _ __   __ _ _   _| |__) || | ___ | |_  |
 | |  ___/ _ \ '_ \ / _` | | | |  ___/ | |/ _ \| __| |
 | | |  |  __/ | | | (_| | |_| | |   | | | (_) | |_  |
 | |_|   \___|_| |_|\__, |\__,_|_|   |_|_|\___/ \__| |
 |                   __/ |                           |
 |  GNU/Linux based |___/  Multi-Rotor UAV Autopilot |
 |___________________________________________________|
  
 Power Management Service
 - monitors system power
 - estimates battery state of charge (SOC)
 - predicts remaining battery lifetime
 - manages main power and lights

 Copyright (C) 2011 Tobias Simon, Ilmenau University of Technology

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details. """


from time import sleep
from threading import Thread, Timer
from signal import pause
from smbus import SMBus
from os import system, sep
from logging import basicConfig as log_config, debug as log_debug
from logging import info as log_info, warning as log_warn, error as log_err
from logging import DEBUG

from misc import *
from power_pb2 import *
from scl import generate_map
from opcd_interface import OPCD_Interface
from misc import daemonize, Hysteresis, user_data_dir
from hardware import ADC, GPIO_Bank


class PowerMan:

   def __init__(self, name):
      # set-up logger:
      logfile = user_data_dir() + sep + 'PowerMan.log'
      log_config(filename = logfile, filemode = 'w', level = DEBUG,
                 format = '%(asctime)s - %(levelname)s: %(message)s')
      # initialized and load config:
      log_info('powerman starting up')
      map = generate_map(name)
      self.ctrl_socket = map['ctrl']
      self.monitor_socket = map['mon']
      self.opcd = OPCD_Interface(map['opcd_ctrl'], 'powerman')
      bus = SMBus(self.opcd.get('gpio_i2c_bus'))
      self.gpio_mosfet = GPIO_Bank(bus, self.opcd.get('gpio_i2c_address'))
      self.power_pin = self.opcd.get('gpio_power_pin')
      self.cells = self.opcd.get('battery_cells')
      self.low_cell_voltage = self.opcd.get('battery_low_cell_voltage')
      self.capacity = self.opcd.get('battery_capacity')
      self.low_battery_voltage = self.cells * self.low_cell_voltage
      self.critical = False
      #self.gpio_mosfet.write()
      self.warning_started = False

      # start threads:
      self.standing = True
      self.adc_thread = start_daemon_thread(self.adc_reader)
      self.emitter_thread = start_daemon_thread(self.power_state_emitter)
      self.request_thread = start_daemon_thread(self.request_handler)
      log_info('powerman running')


   def battery_warning(self):
      # do something in order to indicate a low battery:
      msg = 'CRITICAL WARNING: SYSTEM BATTERY VOLTAGE IS LOW; IMMEDIATE SHUTDOWN REQUIRED OR SYSTEM WILL BE DAMAGED'
      log_warn(msg)
      system('echo "%s" | wall' % msg)
      beeper_enabled = self.opcd.get('beeper_enabled')
      while True:
         if beeper_enabled:
            self.gpio_mosfet.set_gpio(5, False)
            sleep(0.1)
            self.gpio_mosfet.set_gpio(5, True)
            sleep(0.1)
         else:
            sleep(1.0)


   def adc_reader(self):
      voltage_adc = ADC(self.opcd.get('voltage_adc'))
      current_adc = ADC(self.opcd.get('current_adc'))
      voltage_lambda = eval(self.opcd.get('adc_2_voltage'))
      current_lambda = eval(self.opcd.get('adc_2_current'))
      self.current_integral = 0.0
      hysteresis = Hysteresis(self.opcd.get('low_voltage_hysteresis'))
      while True:
         sleep(1)
         try:
            self.voltage = voltage_lambda(voltage_adc.read())  
            self.current = current_lambda(current_adc.read())
            self.current_integral += self.current / 3600
            if self.voltage < self.low_battery_voltage:
               self.critical = hysteresis.set()
            else:
               hysteresis.reset()
            if self.critical:
               if not self.warning_started:
                  self.warning_started = True
                  start_daemon_thread(self.battery_warning)
         except Exception, e:
            log_err(str(e))

   def power_state_emitter(self):
      while True:
         state = PowerState()
         sleep(1)
         try:
            state.voltage = self.voltage
            state.current = self.current
            if state.current < 4.0:
               state.current = 0.5
            state.capacity = self.capacity
            state.consumed = self.current_integral
            remaining = self.capacity - self.current_integral
            if remaining < 0:
               remaining = 0
            state.remaining = remaining
            state.critical = self.critical
            state.estimate = state.remaining / self.current * 3600
            log_info(str(state).replace('\n', ' '))
         except AttributeError:
            continue
         except: # division by zero in last try block line
            pass
         self.monitor_socket.send(state.SerializeToString())


   def power_off(self):
      self.gpio_mosfet.set_gpio(self.power_pin, False)


   def request_handler(self):
      timeout = self.opcd.get('power_save_timeout')
      req = PowerReq()
      rep = PowerRep()
      timer = None
      while True:
         rep.status = OK
         try:
            req_data = self.ctrl_socket.recv()
         except:
            sleep(1)
            continue
         try:
            req.ParseFromString(req_data)
         except:
            rep.status = E_SYNTAX
         else:
            try:
               timer.cancel()
            except:
               pass
            if req.cmd == STAND_POWER:
               timer = Timer(timeout, self.power_off)
               timer.start()
            else:
               if self.critical:
                  # flying command is not allowed if battery was critical:
                  rep.status = E_POWER
               else:
                  self.gpio_mosfet.set_gpio(self.power_pin, True)
         self.ctrl_socket.send(rep.SerializeToString())



def main(name):
   PowerMan(name)
   await_signal()


daemonize('powerman', main)
