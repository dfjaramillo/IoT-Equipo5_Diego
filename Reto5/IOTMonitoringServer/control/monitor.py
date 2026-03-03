from argparse import ArgumentError
import ssl
from django.db.models import Avg
from django.utils import timezone
from datetime import timedelta, datetime
from receiver.models import Data, Measurement
import paho.mqtt.client as mqtt
import schedule
import time
from django.conf import settings

client = None


def analyze_data():
    # Consulta todos los datos de la última hora, los agrupa por estación y variable
    # Compara el promedio con los valores límite que están en la base de datos para esa variable.
    # Si el promedio se excede de los límites, se envia un mensaje de alerta.

    print("Calculando alertas...")

    data = Data.objects.filter(
        base_time__gte=timezone.now() - timedelta(hours=1))
    aggregation = data.annotate(check_value=Avg('avg_value')) \
        .select_related('station', 'measurement') \
        .select_related('station__user', 'station__location') \
        .select_related('station__location__city', 'station__location__state',
                        'station__location__country') \
        .values('check_value', 'station__user__username',
                'measurement__name',
                'measurement__max_value',
                'measurement__min_value',
                'station__location__city__name',
                'station__location__state__name',
                'station__location__country__name')
    alerts = 0
    for item in aggregation:
        alert = False

        variable = item["measurement__name"]
        max_value = item["measurement__max_value"]
        min_value = item["measurement__min_value"]

        # Si no hay límites configurados, no se puede evaluar alerta
        if max_value is None and min_value is None:
            continue

        max_value = max_value or 0
        min_value = min_value or 0

        country = item['station__location__country__name']
        state = item['station__location__state__name']
        city = item['station__location__city__name']
        user = item['station__user__username']

        if item["check_value"] > max_value or item["check_value"] < min_value:
            alert = True

        if alert:
            message = "ALERT {} {} {}".format(variable, min_value, max_value)
            topic = '{}/{}/{}/{}/in'.format(country, state, city, user)
            print(timezone.now(), "Sending alert to {} {}".format(topic, variable))
            client.publish(topic, message)
            alerts += 1

    print(len(aggregation), "dispositivos revisados")
    print(alerts, "alertas enviadas")


def analyze_luminosity_event():
    """
    Evento de luminosidad: Evalúa la luminosidad promedio de la última hora.

    Pre-requisito (consulta a la BD):
        Se consulta la tabla Data filtrando por measurement.name == 'luminosidad'
        en la última hora, agrupando por estación, y se calcula el promedio
        (luminosidad_promedio) mediante una agregación Avg sobre avg_value.

    Condición:
        Si luminosidad_promedio > max_value o luminosidad_promedio < min_value
        (umbrales configurados en la tabla Measurement para 'luminosidad'),
        se considera que hay una alerta.

    Acción:
        Se envía un mensaje MQTT "LUMINOSITY_ALERT ON <promedio> <min> <max>"
        al dispositivo IoT. El MCU interpreta este comando y activa un pin
        GPIO que habilita un circuito oscilador NE555 conectado a dos LEDs,
        haciendo que parpadeen de forma alternada como indicador visual.
        Cuando la condición deja de cumplirse, se envía "LUMINOSITY_ALERT OFF"
        para desactivar el oscilador.
    """
    print("Evaluando evento de luminosidad...")

    # Pre-requisito: Verificar que existe la variable 'luminosidad' en la BD
    lum_measurements = Measurement.objects.filter(name__iexact='luminosidad')

    if not lum_measurements.exists():
        print("No se encontró la variable 'luminosidad' en la base de datos.")
        return

    # Consulta a la BD: Obtener luminosidad promedio de la última hora por estación
    lum_data = Data.objects.filter(
        base_time__gte=timezone.now() - timedelta(hours=1),
        measurement__in=lum_measurements
    ).select_related(
        'station', 'measurement',
        'station__user', 'station__location',
        'station__location__city', 'station__location__state',
        'station__location__country'
    ).values(
        'station__user__username',
        'station__location__city__name',
        'station__location__state__name',
        'station__location__country__name',
        'measurement__min_value',
        'measurement__max_value',
    ).annotate(luminosidad_promedio=Avg('avg_value'))

    alerts = 0
    for item in lum_data:
        luminosidad_promedio = item['luminosidad_promedio']
        max_value = item['measurement__max_value']
        min_value = item['measurement__min_value']

        # Si no hay umbrales configurados, no se evalúa
        if max_value is None and min_value is None:
            continue

        max_value = max_value if max_value is not None else 1024.0
        min_value = min_value if min_value is not None else 0.0

        country = item['station__location__country__name']
        state = item['station__location__state__name']
        city = item['station__location__city__name']
        user = item['station__user__username']
        topic = '{}/{}/{}/{}/in'.format(country, state, city, user)

        # Condición: luminosidad promedio fuera de rango
        if luminosidad_promedio > max_value or luminosidad_promedio < min_value:
            message = "LUMINOSITY_ALERT ON {:.1f} {:.1f} {:.1f}".format(
                luminosidad_promedio, min_value, max_value)
            print(timezone.now(),
                  "Alerta de luminosidad -> {} | Promedio: {:.1f} lx".format(
                      topic, luminosidad_promedio))
            client.publish(topic, message)
            alerts += 1
        else:
            # Desactivar el oscilador si la luminosidad está en rango normal
            message = "LUMINOSITY_ALERT OFF {:.1f}".format(luminosidad_promedio)
            client.publish(topic, message)

    print("{} estaciones evaluadas para evento de luminosidad".format(
        len(lum_data)))
    print("{} alertas de luminosidad enviadas".format(alerts))


def on_connect(client, userdata, flags, rc):
    '''
    Función que se ejecuta cuando se conecta al bróker.
    '''
    print("Conectando al broker MQTT...", mqtt.connack_string(rc))


def on_disconnect(client: mqtt.Client, userdata, rc):
    '''
    Función que se ejecuta cuando se desconecta del broker.
    Intenta reconectar al bróker.
    '''
    print("Desconectado con mensaje:" + str(mqtt.connack_string(rc)))
    print("Reconectando...")
    client.reconnect()


def setup_mqtt():
    '''
    Configura el cliente MQTT para conectarse al broker.
    '''

    print("Iniciando cliente MQTT...", settings.MQTT_HOST, settings.MQTT_PORT)
    global client
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, settings.MQTT_USER_PUB)
        client.on_connect = on_connect
        client.on_disconnect = on_disconnect

        if settings.MQTT_USE_TLS:
            client.tls_set(ca_certs=settings.CA_CRT_PATH,
                           tls_version=ssl.PROTOCOL_TLSv1_2, cert_reqs=ssl.CERT_NONE)

        client.username_pw_set(settings.MQTT_USER_PUB,
                               settings.MQTT_PASSWORD_PUB)
        client.connect(settings.MQTT_HOST, settings.MQTT_PORT)
        client.loop_start()

    except Exception as e:
        print('Ocurrió un error al conectar con el bróker MQTT:', e)


def start_cron():
    '''
    Inicia el cron que se encarga de ejecutar la función analyze_data cada 5 minutos
    y la función analyze_humidity_event cada 5 minutos.
    '''
    print("Iniciando cron...")
    schedule.every(5).minutes.do(analyze_data)
    schedule.every(5).minutes.do(analyze_luminosity_event)
    print("Servicio de control iniciado (alertas generales + evento de luminosidad)")
    while 1:
        schedule.run_pending()
        time.sleep(1)
